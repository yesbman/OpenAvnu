/******************************************************************************

  Copyright (c) 2012 Intel Corporation 
  All rights reserved.
  
  Redistribution and use in source and binary forms, with or without 
  modification, are permitted provided that the following conditions are met:
  
   1. Redistributions of source code must retain the above copyright notice, 
      this list of conditions and the following disclaimer.
  
   2. Redistributions in binary form must reproduce the above copyright 
      notice, this list of conditions and the following disclaimer in the 
      documentation and/or other materials provided with the distribution.
  
   3. Neither the name of the Intel Corporation nor the names of its 
      contributors may be used to endorse or promote products derived from 
      this software without specific prior written permission.
  
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

******************************************************************************/

#include "ieee1588.hpp"
#include "avbts_clock.hpp"
#include "avbts_osnet.hpp"
#include "avbts_oslock.hpp"
#ifdef ARCH_INTELCE
#include "linux_hal_intelce.hpp"
#else
#include "linux_hal_generic.hpp"
#endif
#include <ctype.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "min_port.hpp"
#include "md_ethport.hpp"

void print_usage( char *arg0 ) {
  fprintf( stderr,
	   "%s <network interface> [-S] [-P] [-M <filename>] "
	   "[-A <count>] [-G <group>] [-R <priority 1>]\n",
	   arg0 );
  fprintf
	  ( stderr,
		"\t-S start syntonization\n\t-P pulse per second\n"
		"\t-M <filename> save/restore state\n"
		"\t-A <count> initial accelerated sync count\n"
		"\t-G <group> group id for shared memory\n"
		"\t-R <priority 1> priority 1 value\n" 
		"\t-T force master\n\t-L force slave\n" );
}

int main(int argc, char **argv)
{
	sigset_t set;
	InterfaceName *ifname;
	int sig;

	bool syntonize = false;
	int i;
	bool pps = false;
	uint8_t priority1 = 248;
	bool override_portstate = false;
	PortState port_state;

	int restorefd = -1;
	void *restoredata = ((void *) -1);
	char *restoredataptr = NULL;
	off_t restoredatalength = 0;
	off_t restoredatacount;
	bool restorefailed = false;
	LinuxIPCArg *ipc_arg = NULL;
	
	int accelerated_sync_count = 0;

	// Block SIGUSR1
	{
		sigset_t block;
		sigemptyset( &block );
		sigaddset( &block, SIGUSR1 );
		if( pthread_sigmask( SIG_BLOCK, &block, NULL ) != 0 ) {
			fprintf( stderr, "Failed to block SIGUSR1\n" );
			return -1;
		}
	}
    
	LinuxNetworkInterfaceFactory *default_factory =
		new LinuxNetworkInterfaceFactory;
	OSNetworkInterfaceFactory::registerFactory
		(factory_name_t("default"), default_factory);
	LinuxThreadFactory *thread_factory = new LinuxThreadFactory();
	LinuxTimerQueueFactory *timerq_factory = new LinuxTimerQueueFactory();
	LinuxLockFactory *lock_factory = new LinuxLockFactory();
	LinuxTimerFactory *timer_factory = new LinuxTimerFactory();
	LinuxConditionFactory *condition_factory = new LinuxConditionFactory();
	LinuxSharedMemoryIPC *ipc = new LinuxSharedMemoryIPC();
	/* Create Low level network interface object */
	if( argc < 2 ) {
		printf( "Interface name required\n" );
		print_usage( argv[0] );
		return -1;
	}

	/* Process optional arguments */
	for( i = 2; i < argc; ++i ) {
		if( argv[i][0] == '-' ) {
			if( toupper( argv[i][1] ) == 'S' ) {
				// Get syntonize directive from command line
				syntonize = true;
			}
			else if( toupper( argv[i][1] ) == 'T' ) {
				override_portstate = true;
				port_state = PTP_MASTER;
			}
			else if( toupper( argv[i][1] ) == 'L' ) {
				override_portstate = true;
				port_state = PTP_SLAVE;
			}
			else if( toupper( argv[i][1] ) == 'M' ) {
				// Open file
				if( i+1 < argc ) {
					restorefd = open
						( argv[i+1], O_RDWR|O_CREAT, S_IRUSR|S_IWUSR ); ++i;
					if( restorefd == -1 ) printf
						( "Failed to open restore file\n" );
				} else {
					printf( "Restore file must be specified on "
							"command line\n" );
				}
			}
			else if( toupper( argv[i][1] ) == 'A' ) {
				if( i+1 < argc ) {
					accelerated_sync_count = atoi( argv[++i] );
				} else {
					printf( "Accelerated sync count must be specified on the "
							"command line with A option\n" );
				}
			}
			else if( toupper( argv[i][1] ) == 'G' ) {
				if( i+1 < argc ) {
					ipc_arg = new LinuxIPCArg(argv[++i]);
				} else {
					printf( "Must specify group name on the command line\n" );
				}
			}
			else if( toupper( argv[i][1] ) == 'P' ) {
				pps = true;
			}
			else if( toupper( argv[i][1] ) == 'H' ) {
				print_usage( argv[0] );
				return 0;
			}
			else if( toupper( argv[i][1] ) == 'R' ) {
				if( i+1 >= argc ) {
					printf( "Priority 1 value must be specified on "
							"command line, using default value\n" );
				} else {
					unsigned long tmp = strtoul( argv[i+1], NULL, 0 ); ++i;
					if( tmp == 0 ) {
						printf( "Invalid priority 1 value, using "
								"default value\n" );
					} else {
						priority1 = (uint8_t) tmp;
					}
				}
			}
		}
	}
    
	if( !ipc->init( ipc_arg ) ) {
	  delete ipc;
	  ipc = NULL;
	}
	if( ipc_arg != NULL ) delete ipc_arg;

	if( restorefd != -1 ) {
		// MMAP file
		struct stat stat0;
		if( fstat( restorefd, &stat0 ) == -1 ) {
			printf( "Failed to stat restore file, %s\n", strerror( errno ));
			restoredatalength = 0;
		} else {
			restoredatalength = stat0.st_size;
			if( restoredatalength != 0 ) {
				if(( restoredata = mmap( NULL, restoredatalength,
										 PROT_READ | PROT_WRITE, MAP_SHARED,
										 restorefd, 0 )) == ((void *)-1) ) {
					printf( "Failed to mmap restore file, %s\n",
							strerror( errno ));
				} else {
					restoredatacount = restoredatalength;
					restoredataptr = (char *) restoredata;
				}
			}
		}
	}
	
	if (argc < 2)
		return -1;
	ifname = new InterfaceName(argv[1], strlen(argv[1]));

#ifdef ARCH_INTELCE
	EthernetTimestamper *timestamper = new LinuxTimestamperIntelCE();
#else
	EthernetTimestamper *timestamper = new LinuxTimestamperGeneric();
#endif

	IEEE1588Clock *clock =
	  new IEEE1588Clock( false, syntonize, priority1, timestamper,
			     timerq_factory , lock_factory, ipc );
	
	if( restoredataptr != NULL ) {
	  if( !restorefailed )
	    restorefailed =
	      !clock->restoreSerializedState( restoredataptr, 
					      &restoredatacount );
	  restoredataptr = ((char *)restoredata) +
	    (restoredatalength - restoredatacount);
	}

	MediaIndependentPort *min_port =
		new MediaIndependentPort
		(clock, accelerated_sync_count, syntonize, condition_factory,
		thread_factory, timer_factory, lock_factory);

	MediaDependentPort *md_port = new MediaDependentEtherPort
		(timestamper, ifname, condition_factory, thread_factory,
		timer_factory, lock_factory);

	min_port->setPort(md_port);
	if (!min_port->init_port()) {
		printf("failed to initialize port \n");
		return -1;
	}

	if( restoredataptr != NULL ) {
	  if( !restorefailed ) restorefailed =
	    !min_port->restoreSerializedState( restoredataptr, &restoredatacount );
	  restoredataptr = ((char *)restoredata) +
	    (restoredatalength - restoredatacount);
	}

	if( override_portstate ) {
		min_port->setPortState( port_state );
	}

	// Start PPS if requested
	if( pps ) {
	  if( !timestamper->HWTimestamper_PPS_start()) {
	    printf( "Failed to start pulse per second I/O\n" );
	  }
	}

	min_port->processEvent(POWERUP);

	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset( &set, SIGTERM );
	if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
		perror("pthread_sigmask()");
		return -1;
	}

	if (sigwait(&set, &sig) != 0) {
		perror("sigwait()");
		return -1;
	}

	fprintf(stderr, "Exiting on %d\n", sig);

	// Stop PPS if previously started
	if( pps ) {
	  if( !timestamper->HWTimestamper_PPS_stop()) {
	    printf( "Failed to stop pulse per second I/O\n" );
	  }
	}

	// If port is either master or slave, save clock and then port state
	if( restorefd != -1 ) {
	  if( min_port->getPortState() == PTP_MASTER ||
		  min_port->getPortState() == PTP_SLAVE ) {
	    off_t len;
	    restoredatacount = 0;
	    clock->serializeState( NULL, &len );
	    restoredatacount += len;
	    min_port->serializeState( NULL, &len );
	    restoredatacount += len;
	
	    if( restoredatacount > restoredatalength ) {
	      ftruncate( restorefd, restoredatacount );
	      if( restoredata != ((void *) -1)) {
		restoredata =
		  mremap( restoredata, restoredatalength, restoredatacount,
			  MREMAP_MAYMOVE );
	      } else {
		restoredata =
		  mmap( NULL, restoredatacount, PROT_READ | PROT_WRITE,
			MAP_SHARED, restorefd, 0 );
	      }
	      if( restoredata == ((void *) -1 )) goto remap_failed;
	      restoredatalength = restoredatacount;
	    }
	    
	    restoredataptr = (char *) restoredata;
	    clock->serializeState( restoredataptr, &restoredatacount );
	    restoredataptr = ((char *)restoredata) +
	      (restoredatalength - restoredatacount);
	    min_port->serializeState( restoredataptr, &restoredatacount );
	    restoredataptr = ((char *)restoredata) +
	      (restoredatalength - restoredatacount);
	  remap_failed:
	    ;;
	  }
	  
      
	  if( restoredata != ((void *) -1 ))
	    munmap( restoredata, restoredatalength );
	}

	if( ipc ) delete ipc;

	return 0;
}