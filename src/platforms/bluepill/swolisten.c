/*
 * SWO Splitter for Blackmagic Probe and others.
 * =============================================
 *
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2017  Dave Marples  <dave@marples.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <libusb.h>
#include <stdint.h>
#include <limits.h>

#define VID       (0x1d50)
#define PID       (0x6018)
#define INTERFACE (5)
#define ENDPOINT  (0x85)

#define TRANSFER_SIZE (64)
#define NUM_FIFOS     32
#define MAX_FIFOS     128

#define CHANNELNAME   "chan"

#define BOOL       char
#define FALSE      (0)
#define TRUE       (!FALSE)

// Record for options, either defaults or from command line
struct
{
  BOOL verbose;
  int nChannels;
  char *chanPath;
} options = {.nChannels=NUM_FIFOS, .chanPath=""};

// Runtime state
struct
{
  int fifo[MAX_FIFOS];
} _r;

// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Internals
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
static BOOL _runFifo(int portNo, int listenHandle, char *fifoName)

{
  int pid,fifo;
  int readDataLen, writeDataLen;

  if (mkfifo(fifoName,0666)<0)
    {
      return FALSE;
    }

  pid=fork();

  if (pid==0)
    {
      char rxdata[TRANSFER_SIZE];
      int fifo;
      
      while (1)
	{
	  /* This is the child */
	  fifo=open(fifoName,O_WRONLY);

	  /* Don't kill this sub-process when any reader or writer evaporates */
	  signal(SIGPIPE, SIG_IGN);

	  while (1)
	    {
	      readDataLen=read(listenHandle,rxdata,TRANSFER_SIZE);
	      if (readDataLen<=0)
		{
		  exit(0);
		}

	      writeDataLen=write(fifo,rxdata,readDataLen);
	      if (writeDataLen<=0)
		{
		  break;
		}
	    }
	  close(fifo);
	}
    } 
  else if (pid<0)
    {
      /* The fork failed */
      return FALSE;
    }
  return TRUE;
}
// ====================================================================================================
static BOOL _makeFifoTasks(void)

/* Create each sub-process that will handle a port */

{
  char fifoName[PATH_MAX];

  int f[2];

  for (int t=0; t<options.nChannels; t++)
    {
      if (pipe(f)<0)
	  return FALSE;
      fcntl(f[1],F_SETFL,O_NONBLOCK);
      _r.fifo[t]=f[1];
      sprintf(fifoName,"%s%s%02X",options.chanPath,CHANNELNAME,t);
      if (!_runFifo(t,f[0],fifoName))
	{
	  return FALSE;
	}
    }
  return TRUE;
}
// ====================================================================================================
static void _removeFifoTasks(void)

/* Destroy the per-port sub-processes */

{
  int statloc;
  int remainingProcesses=0;
  char fifoName[PATH_MAX];

  for (int t=0; t<options.nChannels; t++)
    {
      if (_r.fifo[t]>0)
	{
	  close(_r.fifo[t]);
	  sprintf(fifoName,"%s%s%02X",options.chanPath,CHANNELNAME,t);
	  unlink(fifoName);
	  remainingProcesses++;
	}
    }

  while (remainingProcesses--)
    {
      waitpid(-1,&statloc,0);
    }
}
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Handlers for each message type
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void _handleSWIT(uint8_t addr, uint8_t length, uint8_t *d)

{
  if (addr<options.nChannels)
    write(_r.fifo[addr],d,length);

  //  if (addr==0)
  //  fprintf(stdout,"%c",*d);
}
// ====================================================================================================
void _handleTS(uint8_t length, uint8_t *d)

{

}
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Protocol pump for decoding messages
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
enum _protoState {ITM_IDLE, ITM_SYNCING, ITM_TS, ITM_SWIT};

#ifdef PRINT_TRANSITIONS
static char *_protoNames[]={"IDLE", "SYNCING","TS","SWIT"};
#endif

void _protocolPump(uint8_t *c)

{
  static enum _protoState p;
  static int targetCount, currentCount, srcAddr;
  static uint8_t rxPacket[5];

#ifdef PRINT_TRANSITIONS
  printf("%02x %s --> ",*c,_protoNames[p]);
#endif

  switch (p)
    {
      // -----------------------------------------------------
    case ITM_IDLE:
      if (*c==0b01110000)
	{
	  /* This is an overflow packet */
	  if (options.verbose)
	    fprintf(stderr,"Overflow!\n");
	  break;
	}
      // **********
      if (*c==0)
	{
	  /* This is a sync packet - expect to see 4 more 0's followed by 0x80 */
	  targetCount=4;
	  currentCount=0;
	  p=ITM_SYNCING;
	  break;
	}
      // **********
      if (!(*c&0x0F))
	{
	  currentCount=1;
	  /* This is a timestamp packet */
	  rxPacket[0]=*c;

	  if (!(*c&0x80))
	    {
	      /* A one byte output */
	      _handleTS(currentCount,rxPacket);
	    }
	  else
	    {
	      p=ITM_TS;
	    }
	  break;
	}
      // **********
      if ((*c&0x0F) == 0x04)
	{
	  /* This is a reserved packet */
	  break;
	}
      // **********
      if (!(*c&0x04))
	{
	  /* This is a SWIT packet */
	  if ((targetCount=*c&0x03)==3)
	    targetCount=4;
	  srcAddr=(*c&0xF8)>>3;
	  currentCount=0;
	  p=ITM_SWIT;
	  break;
	}
      // **********
      if (options.verbose)
	fprintf(stderr,"Illegal packet start in IDLE state\n");
      break;
      // -----------------------------------------------------
    case ITM_SWIT:
	  rxPacket[currentCount]=*c;
	  currentCount++;

	  if (currentCount>=targetCount)
	    {
	      p=ITM_IDLE;
	      _handleSWIT(srcAddr, targetCount, rxPacket);
	    }
	  break;
      // -----------------------------------------------------
    case ITM_TS:
      rxPacket[currentCount++]=*c;
      if (!(*c&0x80))
	{
	  /* We are done */
	  _handleTS(currentCount,rxPacket);
	}
      else
	{
	  if (currentCount>4)
	    {
	      /* Something went badly wrong */
	      p=ITM_IDLE;
	    }
	  break;
	}

      // -----------------------------------------------------
    case ITM_SYNCING:
      if ((*c==0) && (currentCount<targetCount))
	{
	  currentCount++;
	}
      else
	{
	  if (*c==0x80)
	    {
	      p=ITM_IDLE;
	    }
	  else
	    {
	      /* This should really be an UNKNOWN state */
	      p=ITM_IDLE;
	    }
	}
      break;
      // -----------------------------------------------------
    }
#ifdef PRINT_TRANSITIONS
  printf("%s\n",_protoNames[p]);
#endif
}
// ====================================================================================================
void intHandler(int dummy) 

{
  exit(0);
}
// ====================================================================================================
void _printHelp(char *progName)

{
  printf("Useage: %s <dhnv> <b basedir>\n",progName);
  printf("        h: This help\n");
  printf("        n: <Number> of channels to populate\n");
  printf("        v: Verbose mode\n");
  printf("        b: <basedir> for channels\n");
}
// ====================================================================================================
int _processOptions(int argc, char *argv[])

{
  int c;
  while ((c = getopt (argc, argv, "vdn:b:h")) != -1)
    switch (c)
      {
      case 'v':
        options.verbose = 1;
        break;
      case 'h':
	_printHelp(argv[0]);
	return FALSE;
      case 'n':
	options.nChannels=atoi(optarg);
	if ((options.nChannels<1) || (options.nChannels>MAX_FIFOS))
	  {
	    fprintf(stderr,"Number of channels out of range (1..%d)\n",MAX_FIFOS);
	    return FALSE;
	  }
	break;
      case 'b':
        options.chanPath = optarg;
        break;
      case '?':
        if (optopt == 'b')
          fprintf (stderr, "Option '%c' requires an argument.\n", optopt);
        else if (!isprint (optopt))
	    fprintf (stderr,"Unknown option character `\\x%x'.\n", optopt);
	return FALSE;
      default:
        return FALSE;
      }

  if (options.verbose)
    {
      fprintf(stdout,"Verbose: TRUE\nBasePath: %s\n",options.chanPath);
    }
  return TRUE;
}
// ====================================================================================================
int main(int argc, char *argv[])
{
  libusb_device_handle *handle;
  libusb_device *dev;
  int size;
  char fifoName[]=CHANNELNAME;

  unsigned char cbw[TRANSFER_SIZE];

  if (!_processOptions(argc,argv))
    {
      exit(-1);
    }

  atexit(_removeFifoTasks);
  /* This ensures the atexit gets called */
  signal(SIGINT, intHandler);
  if (!_makeFifoTasks())
    {
      fprintf(stderr,"Failed to make channel devices\n");
      exit(-1);
    }

  while (1)
    {
      if (libusb_init(NULL) < 0)
	{
	  fprintf(stderr,"Failed to initalise USB interface\n");
	  exit(-1);
	}

      while (!(handle = libusb_open_device_with_vid_pid(NULL, VID, PID)))
	{
	  usleep(500000);
	}

      if (!(dev = libusb_get_device(handle)))
	continue;

      if (libusb_kernel_driver_active(handle, 0))
	{
	  libusb_detach_kernel_driver(handle, 0);
	}
      
      if (libusb_claim_interface (handle, INTERFACE)<0)
	continue;

      while (0==libusb_bulk_transfer(handle, ENDPOINT, cbw, TRANSFER_SIZE, &size, 10))
	{
	  uint8_t *c=cbw;
	  while (size--)
	    _protocolPump(c++);
	}

      libusb_close(handle);
    }

  return 0;
}
// ====================================================================================================
