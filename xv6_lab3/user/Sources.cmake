# Files in the "User-space library"  These are common routines (like system
#    call definitions) that user-space programs will use
set(ulib_SOURCES
  # Contains all of the system calls
  asm/usys.S
  asm/free_stack_and_exit.S

  # Common library functions
  src/ulib.c
  src/umalloc.c
  src/printf.c
  src/threads.c
  )

# User-space programs, 
set(user_SOURCES
  # Init -- the first program the kernel launchs
  src/init.c

  # The shell (launched by init)
  src/sh.c

  # Common utility programs
  src/ls.c
  src/cat.c
  src/schedtest.c

  # Benchmark
  src/benchmark/mmult.c
  src/benchmark/compress.c
  src/benchmark/ping.c
  src/benchmark/networkd.c
  src/workload.c
  )

