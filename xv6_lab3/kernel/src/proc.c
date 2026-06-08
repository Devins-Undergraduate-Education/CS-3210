#include "asm/x86.h"
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sched.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static uint fifo_runq_seq;
static uint rr_runq_seq;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
static void make_runnable(struct proc *p);
static void consider_preemption(struct proc *candidate);
static void assign_runq_slot(struct proc *p);
static struct proc *pick_next_proc(void);
static int proc_is_better(struct proc *a, struct proc *b);
static int policy_rank(int policy);
static int wait_internal(struct schedinfo *uinfo);
#define OTHER_IO_BONUS_SHIFT 1
#define OTHER_WAIT_BOOST_SHIFT 2
static uint now_ticks(void);
static void begin_run(struct proc *p, uint now);
static void finish_run(struct proc *p, uint now);
static void begin_wait(struct proc *p, uint now);
static void finish_wait(struct proc *p, uint now);
static void begin_io(struct proc *p, uint now);
static void finish_io(struct proc *p, uint now);
static uint other_effective_runtime(struct proc *p);
static uint other_wait_bonus(struct proc *p, uint now);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

static uint now_ticks(void) {
  uint t;
  pushcli();
  t = ticks;
  popcli();
  return t;
}

static void begin_run(struct proc *p, uint now) {
  if (p)
    p->last_run_tick = now;
}

static void finish_run(struct proc *p, uint now) {
  if (p && p->last_run_tick != 0) {
    p->sinfo.execution_time += now - p->last_run_tick;
    p->last_run_tick = 0;
  }
}

static void begin_wait(struct proc *p, uint now) {
  if (p) p->last_wait_tick = now;
}

static void finish_wait(struct proc *p, uint now) {
  if (p && p->last_wait_tick != 0) {
    p->sinfo.wait_time += now - p->last_wait_tick;
    p->last_wait_tick = 0;
  }
}

static void begin_io(struct proc *p, uint now) {
  if (p) p->last_io_tick = now;
}

static void finish_io(struct proc *p, uint now) {
  if (p && p->last_io_tick != 0) {
    p->sinfo.io_time += now - p->last_io_tick;
    p->last_io_tick = 0;
  }
}

static void reset_sched_accounting(struct proc *p) {
  if (p == 0) return;
  memset(&p->sinfo, 0, sizeof(p->sinfo));
  p->last_run_tick = 0;
  p->last_wait_tick = 0;
  p->last_io_tick = 0;
}

static uint other_effective_runtime(struct proc *p) {
  if (p == 0) return 0;

  uint exec = p->sinfo.execution_time;
  uint bonus = p->sinfo.io_time >> OTHER_IO_BONUS_SHIFT;

  if (bonus >= exec) return 0;

  return exec - bonus;
}

static uint other_wait_bonus(struct proc *p, uint now) {
  if (p == 0) return 0;

  uint wait = p->sinfo.wait_time;
  if (p->state == RUNNABLE && p->last_wait_tick != 0 && now > p->last_wait_tick)
    wait += now - p->last_wait_tick;

  return wait >> OTHER_WAIT_BOOST_SHIFT;
}

static void assign_runq_slot(struct proc *p) {
  if (p == 0) return;
  if (p->sched_policy == SCHED_FIFO)
    p->fifo_seq = fifo_runq_seq++;
  else
    p->rr_seq = rr_runq_seq++;
  p->need_resched = 0;
}

static void make_runnable(struct proc *p) {
  if (p == 0)
    return;
  uint now = now_ticks();
  if (p->state == RUNNING)
    finish_run(p, now);
  else if (p->state == SLEEPING)
    finish_io(p, now);
  if (p->state != RUNNABLE)
    begin_wait(p, now);
  assign_runq_slot(p);
  p->state = RUNNABLE;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->sched_policy = SCHED_RR;
  p->priority     = 0;
  p->fifo_seq     = 0;
  p->rr_seq       = 0;
  p->need_resched = 0;
  p->proc_head    = p;

  release(&ptable.lock);

  reset_sched_accounting(p);
  p->sinfo.creation_time = now_ticks();

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  p->proc_head = p;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  make_runnable(p);

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  struct proc *p = myproc();
  acquire(&ptable.lock);
  uint sz = p->proc_head->sz;

  if(n > 0){
    if((sz = allocuvm(p->proc_head->pgdir, sz, sz + n)) == 0) {
      release(&ptable.lock);
      return -1;
    }
  } else if(n < 0){
    if((sz = deallocuvm(p->proc_head->pgdir, sz, sz + n)) == 0) {
      release(&ptable.lock);
      return -1;
    }
  }
  p->proc_head->sz = sz;
  release(&ptable.lock);
  switchuvm(p);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();
  struct proc *leader = thread_group_leader(curproc);

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from parent.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }

  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;
  np->proc_head = np;

  // Child returns 0 from fork().
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(leader->ofile[i])
      np->ofile[i] = filedup(leader->ofile[i]);
  np->cwd = idup(leader->cwd);

  // pinit();

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));
  pid = np->pid;

  acquire(&ptable.lock);
  make_runnable(np);
  consider_preemption(np);

  release(&ptable.lock);

  return pid;
}

static inline int is_kernel_addr(uint a) {
  return a >= KERNBASE;
}

static int validate_user_stack(void *stack, int stack_size, pde_t *pgdir, uint procsz) {
  if (!stack || stack_size <= 0) return -1;
  uint start = (uint)stack;
  uint end   = (uint)((char*)stack + stack_size);
  if (is_kernel_addr(start) || is_kernel_addr(end)) return -1;
  if (end < start) return -1; // overflow guard
  if (end > procsz) return -1;

  // per-page mapping+writable check
  for (uint a = PGROUNDDOWN(start); a < end; a += PGSIZE) {
    pte_t *pte = walkpgdir(pgdir, (char*)a, 0);
    if (!pte || !(*pte & PTE_P) || !(*pte & PTE_W)) return -1;
  }
  return 0;
}

static void patch_child_frames(uint *parent_ebp, uint *parent_top_ebp, int offset) {
  for (uint *ebp = parent_ebp;; ebp = (uint*)(*ebp)) {
    uint *child_stack_ebp_ptr = (uint*)((uint)ebp + offset);
    child_stack_ebp_ptr[0] = ebp[0] + offset;
    if (ebp == parent_top_ebp) break;
  }
}

int clone(void *stack, int stack_size)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();
  struct proc *leader = thread_group_leader(curproc);

  if (validate_user_stack(stack, stack_size, curproc->pgdir, curproc->sz) < 0)
    return -1;

  uint *ebp = (uint *)curproc->tf->ebp;
  for (;;) {
    if (is_kernel_addr((uint)ebp) || is_kernel_addr((uint)(ebp + 1)))
      return -1;
    if (*(ebp + 1) == 0xFFFFFFFFU)
      break; // top frame found
    uint *next = (uint *)(*ebp);
    if (is_kernel_addr((uint)next))
      return -1;
    ebp = next;
  }
  uint *parent_top_ebp = ebp;

  uint parent_sp = curproc->tf->esp;
  if (is_kernel_addr(parent_sp) || parent_sp > (uint)parent_top_ebp)
    return -1;
  int parent_stack_size = ((uint)parent_top_ebp - parent_sp) + 8;
  if (parent_stack_size <= 0) return -1;
  if (stack_size < parent_stack_size) return -1;

  uint child_sp = (uint)((char*)stack + stack_size) - parent_stack_size;

  if (!((child_sp + parent_stack_size) <= parent_sp ||
        (parent_sp + parent_stack_size) <= child_sp))
    return -1;

  if (child_sp < (uint)stack) return -1;

  if ((np = allocproc()) == 0)
    return -1;

  int offset = (int)child_sp - (int)parent_sp;

  memmove((void *)child_sp, (void *)parent_sp, parent_stack_size);

  patch_child_frames((uint *)curproc->tf->ebp, parent_top_ebp, offset);

  np->proc_head = leader;
  np->pgdir = curproc->pgdir;   // share address space
  np->parent = curproc;

  *np->tf = *curproc->tf;
  np->tf->eax = 0;
  np->sz = curproc->sz;

  np->tf->esp = child_sp;
  np->tf->ebp = curproc->tf->ebp + offset;

  for (i = 0; i < NOFILE; i++)
    np->ofile[i] = leader->ofile[i];

  np->cwd = leader->cwd;

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;
  acquire(&ptable.lock);
  make_runnable(np);
  consider_preemption(np);
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *cur = myproc();
  struct proc *leader = thread_group_leader(cur);
  struct proc *p;
  int fd;

  if(cur == initproc)
    panic("init exiting");

  if (leader == 0)
    leader = cur;
  int is_leader = (leader == cur);

  if (is_leader) {
    for (fd = 0; fd < NOFILE; fd++) {
      struct file *f = leader->ofile[fd];
      if (f) {
        fileclose(f);
        leader->ofile[fd] = 0;
      }
    }
  }
  for (fd = 0; fd < NOFILE; fd++)
    cur->ofile[fd] = 0;

  if (is_leader) {
    begin_op();
    iput(cur->cwd);
    end_op();
  }
  cur->cwd = 0;

  acquire(&ptable.lock);

  wakeup1(cur->parent);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == cur){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  uint now = now_ticks();
  finish_run(cur, now);
  cur->sinfo.exit_time = now;
  cur->sinfo.response_time = now - cur->sinfo.creation_time;
  cur->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

static int wait_internal(struct schedinfo *uinfo) { // tiny refactor :)
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one to reap.
        pid = p->pid;
        struct schedinfo sinfo = p->sinfo;
        kfree(p->kstack);
        p->kstack = 0;

        if (p->proc_head == p) freevm(p->pgdir);

        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        reset_sched_accounting(p);

        release(&ptable.lock);
        if (uinfo && copyout(curproc->pgdir, (uint)uinfo, &sinfo,
                             sizeof(sinfo)) < 0)
          return -1;
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(curproc, &ptable.lock);
  }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void) {
  return wait_internal(0);
}

int waitinfo(struct schedinfo *uinfo) {
  return wait_internal(uinfo);
}

int
waitpid(int pid)
{
  struct proc *p;
  int havekids;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      if(p->pid == pid) {
        havekids = 1;
        if(p->state == ZOMBIE){
        // Found one to reap.
        // pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;

        if (p->proc_head == p) freevm(p->pgdir);

        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        reset_sched_accounting(p);

        release(&ptable.lock);
        return 0;
      }
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(curproc, &ptable.lock);
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void) {
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    sti();

    acquire(&ptable.lock);
    struct proc *p = pick_next_proc();
    if (p != 0) {
      c->proc = p;
      switchuvm(p);
      uint now = now_ticks();
      finish_wait(p, now);
      begin_run(p, now);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      c->proc = 0;
    }
    release(&ptable.lock);
  }
}


// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&ptable.lock);  //DOC: yieldlock
  make_runnable(p);
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  uint now = now_ticks();
  finish_run(p, now);
  begin_io(p, now);
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; ++p) {
    if (p->state == SLEEPING && p->chan == chan) {
      make_runnable(p);
      consider_preemption(p);
    }
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING){
        make_runnable(p);
        consider_preemption(p);
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
static int
policy_rank(int policy)
{
  switch (policy) {
  case SCHED_FIFO:    return 3;
  case SCHED_RR:    return 2;
  case SCHED_OTHER:    return 1;
  default:    return 0;
  }
}

static int proc_is_better(struct proc *a, struct proc *b) {
  if (a == 0)    return 0;
  if (b == 0)    return 1;

  int rank_a = policy_rank(a->sched_policy);
  int rank_b = policy_rank(b->sched_policy);
  if (rank_a != rank_b)    return rank_a > rank_b;

  if (a->priority != b->priority)    return a->priority > b->priority;

  if (a->sched_policy == SCHED_FIFO) {
    if (a->fifo_seq != b->fifo_seq)      return a->fifo_seq < b->fifo_seq;
  } else if (a->sched_policy == SCHED_RR) {
    if (a->rr_seq != b->rr_seq)
      return a->rr_seq < b->rr_seq;
  } else if (a->sched_policy == SCHED_OTHER) {
    uint now = now_ticks();
    uint a_exec = other_effective_runtime(a);
    uint b_exec = other_effective_runtime(b);
    if (a_exec != b_exec)      return a_exec < b_exec;
    uint a_wait = other_wait_bonus(a, now);
    uint b_wait = other_wait_bonus(b, now);
    if (a_wait != b_wait)      return a_wait > b_wait;
    if (a->rr_seq != b->rr_seq)      return a->rr_seq < b->rr_seq;
  }

  return a->pid < b->pid;
}

static struct proc * pick_next_proc(void) {
  struct proc *best = 0;
  for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state != RUNNABLE)
      continue;
    if (proc_is_better(p, best))
      best = p;
  }
  return best;
}

static void
consider_preemption(struct proc *candidate)
{
  if (candidate == 0 || candidate->state != RUNNABLE)
    return;

  struct proc *current = myproc();
  if (current == 0 || current == candidate)
    return;
  if (current->state != RUNNING)
    return;

  if (!proc_is_better(candidate, current))
    return;

  current->need_resched = 1;
}

int kernel_setscheduler(int pid, int policy, int priority) {
  if (priority < 0)
    return -1;
  if (policy != SCHED_RR && policy != SCHED_FIFO && policy != SCHED_OTHER)
    return -1;

  struct proc *curproc = myproc();
  struct proc *target = 0;

  acquire(&ptable.lock);
  for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      target = p;
      break;
    }
  }

  if (target == 0) {
    release(&ptable.lock);
    return -1;
  }

  if (!(target == curproc || target->parent == curproc)) {
    release(&ptable.lock);
    return -1;
  }

  target->sched_policy = policy;
  target->priority = priority;

  if (target->state == RUNNABLE)
    assign_runq_slot(target);

  consider_preemption(target);
  release(&ptable.lock);
  return 0;
}


int park(void *chan) {
  struct proc *p = myproc();
  if (!p) return -1;

  acquire(&ptable.lock);

  if (p->idek_where_i_wokeup) {
    p->idek_where_i_wokeup = 0;
    p->hmmmm_channel = 0;
    release(&ptable.lock);
    return 0;
  }

  p->hmmmm_channel = 0;
  sleep(chan, &ptable.lock);
  release(&ptable.lock);
  return 0;
}

int setpark(void *chan) {
  struct proc *p = myproc();
  if (!p) return -1;

  acquire(&ptable.lock);
  p->hmmmm_channel = chan;
  release(&ptable.lock);
  return 0;
}

static int wake_first_sleeping_on(void *chan) {
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == SLEEPING && p->chan == chan) {
      p->chan = 0;
      make_runnable(p);
      consider_preemption(p);
      return 1;
    }
  }
  return 0;
}

int unpark(void *chan) {
  struct proc *p;
  int awakened = 0;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->hmmmm_channel == chan) {
      p->hmmmm_channel = 0;
      p->idek_where_i_wokeup = 1;
      awakened++;
    }
  }

  if (awakened == 0) {
    awakened += wake_first_sleeping_on(chan);
  }

  release(&ptable.lock);
  return awakened;
}

struct proc* thread_group_leader(struct proc *p) {
  if (p == 0)
    return 0;
  while (p->proc_head && p->proc_head != p)
    p = p->proc_head;
  return p;
}

void thread_group_set_cwd(struct proc *leader, struct inode *cwd)
{
  struct proc *root = thread_group_leader(leader);
  if (root == 0)
    return;
  acquire(&ptable.lock);
  for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (thread_group_leader(p) == root)
      p->cwd = cwd;
  }
  release(&ptable.lock);
}