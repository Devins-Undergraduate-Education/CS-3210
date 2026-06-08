#include "asm/x86.h"
#include "types.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "sched.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_waitinfo(void)
{
  struct schedinfo *uinfo;
  if(argptr(0, (void*)&uinfo, sizeof(*uinfo)) < 0)
    return -1;
  return waitinfo(uinfo);
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  struct proc *leader = thread_group_leader(myproc());
  addr = leader ? leader->sz : 0;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

int
sys_setscheduler(void)
{
  int pid, policy, priority;
  if (argint(0, &pid) < 0)      return -1;
  if (argint(1, &policy) < 0)   return -1;
  if (argint(2, &priority) < 0) return -1;

  return kernel_setscheduler(pid, policy, priority);
}

// sysproc.c
int
sys_clone(void)
{
  void *stack;
  int stack_size;
  if (argptr(0, (void*)&stack, sizeof(void*)) < 0) return -1;
  if (argint(1, &stack_size) < 0) return -1;

  if (stack == 0)
    return -1;
  if (stack_size < PGSIZE)
    return -1;

  return clone(stack, stack_size);
}

int
sys_waitpid(void)
{
  int pid;
  if(argint(0, &pid) < 0)
    return -1;
  return waitpid(pid);
}


int
sys_park(void)
{
  void *chan;
  if (argptr(0, (char **)&chan, sizeof(chan)) < 0) return -1;
  return park(chan);
}

int
sys_setpark(void)
{
  void *chan;
  if (argptr(0, (char **)&chan, sizeof(chan)) < 0) return -1;
  return setpark(chan);
}

int
sys_unpark(void)
{
  void *chan;
  if (argptr(0, (char **)&chan, sizeof(chan)) < 0) return -1;
  return unpark(chan);
}
