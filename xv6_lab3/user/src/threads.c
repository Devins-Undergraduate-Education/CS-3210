#include "threads.h"
#include "atomics.h"
#include "user.h"
#include "memlayout.h" // need this for sedcuiry
#include "free_stack_and_exit.h" // ty tas
#include "param.h"

struct thread_stack_entry {
  int pid;
  void *stack;
};

static struct thread_stack_entry thread_stacks[NPROC];
static atomic_int thread_stack_lock = ATOMIC_VAR_INIT(0);

static void lock_thread_stack_table(void) {
  while (atomic_exchange_explicit(&thread_stack_lock, 1, memory_order_acquire) != 0) {}
}

static void unlock_thread_stack_table(void) {
  atomic_store_explicit(&thread_stack_lock, 0, memory_order_release);
}

static void register_thread_stack(int pid, void *stack) {
  if (pid <= 0 || stack == 0) {
    exit();
  }

  lock_thread_stack_table();
  int free_slot = -1;
  for (int i = 0; i < NPROC; i++) {
    if (thread_stacks[i].pid == pid) {
      thread_stacks[i].stack = stack;
      unlock_thread_stack_table();
      return;
    }
    if (free_slot < 0 && thread_stacks[i].pid == 0)
      free_slot = i;
  }

  if (free_slot >= 0) {
    thread_stacks[free_slot].pid = pid;
    thread_stacks[free_slot].stack = stack;
    unlock_thread_stack_table();
    return;
  }

  unlock_thread_stack_table();
  exit();
}

static void *consume_thread_stack(int pid) {
  lock_thread_stack_table();
  for (int i = 0; i < NPROC; i++) {
    if (thread_stacks[i].pid == pid) {
      void *stack = thread_stacks[i].stack;
      thread_stacks[i].pid = 0;
      thread_stacks[i].stack = 0;
      unlock_thread_stack_table();
      return stack;
    }
  }
  unlock_thread_stack_table();
  return 0;
}

static void thread_exit_with_stack(void *stack) __attribute__((noreturn));
static void thread_exit_with_stack(void *stack) {
  int pid = getpid();
  void *registered = consume_thread_stack(pid);
  if (registered)
    stack = registered;

  if (stack == 0) {
    exit();
  }
  free_stack_and_exit(stack);
}

static inline int is_User_spacerino(void *base, uint sz) {
  return (uint)base < KERNBASE && (uint)(base + sz) < KERNBASE;
}

int thread_create(void *(*start_routine)(void *), void *arg) {
  void *stack = malloc(4096);
  if(stack == 0) return -1; // failed

  if(!is_User_spacerino(stack, 4096) || (uint)start_routine >= KERNBASE) {
    free(stack);
    return -1;
  }

  int pid = clone(stack, 4096);
  if (pid < 0) {
    free(stack);
    return -1;
  }

  if (pid == 0) {
    register_thread_stack(getpid(), stack);
    start_routine(arg);
    thread_exit_with_stack(stack);
  }

  return pid;
}

int thread_wait(int pid) {
  int r = waitpid(pid);
  if (r < 0) {
    return -1;
  }
  return pid;
}

int spinlock_init(struct spinlock* s) {
  if (!s) return -1;

  atomic_init(&s->lock, 0);
  atomic_store_explicit(&s->start, 1, memory_order_release);
  return 0;
}

int spinlock_acquire(struct spinlock* s) {
  if (!s) return -1;
  if (atomic_load_explicit(&s->start, memory_order_acquire) != 1) return -1;

  while (atomic_exchange_explicit(&s->lock, 1, memory_order_acquire) != 0) {}
  return 0;
}

int spinlock_release(struct spinlock* s) {
  if (!s) return -1;
  if (atomic_load_explicit(&s->start, memory_order_acquire) != 1) return -1;
  
  atomic_store_explicit(&s->lock, 0, memory_order_release);
  return 0;
}

int mutex_init(struct mutex* m) {
  if (!m) return -1;

  atomic_init(&m->lock, 0);
  m->channel = (void *)m;
  return 0;
}

int mutex_acquire(struct mutex* m) {
  if (!m) return -1;

  for (;;) {
    if (atomic_exchange(&m->lock, 1) == 0) return 0;
    if (park(m->channel) < 0) return -1;
  }
}

int mutex_release(struct mutex* m) {
  if (!m) return -1;

  if (atomic_load(&m->lock) == 0) return -1;

  atomic_store(&m->lock, 0);
  if (unpark(m->channel) < 0) return -1;
  return 0;
}

int cond_init(struct condvar *cond) {
  if (!cond) return -1;

  cond->channel = (void *)cond;
  return 0;
}

int cond_wait(struct condvar *cond, struct mutex *m) {
  if (!cond || !m) return -1;

  if (mutex_release(m) < 0) return -1;
  if (park(cond->channel) < 0) return -1;
  if (mutex_acquire(m) < 0) return -1;
 
  return 0;
}

int cond_signal(struct condvar *cond) {
  if (!cond) return -1;

  if (unpark(cond->channel) < 0) return -1;
  return 0;
}

void thread_exit(void) {
  thread_exit_with_stack(0);
}
