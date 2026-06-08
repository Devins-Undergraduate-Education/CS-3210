#ifndef __INCLUDE_sched_h
#define __INCLUDE_sched_h

#include "types.h"

#define SCHED_FIFO 0
#define SCHED_RR 1
#define SCHED_OTHER 2

struct schedinfo 
{
  uint creation_time;  // time when the process was created
  uint exit_time;      // time when the process exited
  uint response_time;  // time from creation to exit (user-centric measure)
  uint execution_time; // time spend executing on a cpu
  uint wait_time;      // time spent waiting in ready queue
  uint io_time;        // time spend waiting for and executing in I/O 
};

#endif

