#include <stdatomic.h>
#include "sched.h"

// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  atomic_uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  int sched_policy;            // SCHED_RR or SCHED_FIFO
  int priority;                // >= 0, higher = more important
  uint fifo_seq;               // FIFO run-queue order token
  uint rr_seq;                 // RR run-queue order token
  struct proc *proc_head;      // leader of the proc
  void *hmmmm_channel;         // chan
  int idek_where_i_wokeup;     // literally no clue where we are
  int need_resched;            // request to yield when safe
  struct schedinfo sinfo;      // scheduling statistics
  uint last_run_tick;          // tick running
  uint last_wait_tick;         // tick runnable
  uint last_io_tick;           // tick sleep
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap

// this is a work around for circular imports... 
int kernel_setscheduler(int pid, int policy, int priority);
int clone(void *u_stack, int stack_size);
int waitpid(int pid);
void acquire_ptable(void);
void release_ptable(void);
int unpark(void *chan);
int setpark(void *chan);
int park(void *chan);
pte_t * walkpgdir(pde_t *pgdir, const void *va, int alloc);