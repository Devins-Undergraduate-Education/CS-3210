#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "threads.h"
#include "atomics.h"

#ifndef MIN_STACK
#define MIN_STACK 4096
#endif

#define ASSERT_TRUE(expr, msg) do { \
  if (!(expr)) { \
    printf(1, "FAIL: %s (line %d)\n", msg, __LINE__); \
    exit(); \
  } \
} while (0)

#define ASSERT_EQ(a,b,msg) do { \
  if ((a) != (b)) { \
    printf(1, "FAIL: %s (got %d, expected %d) (line %d)\n", msg, (a), (b), __LINE__); \
    exit(); \
  } \
} while (0)

#define PASS(name) printf(1, "PASS: %s\n", name)

#define CHILD_FAIL(msg) do { \
  printf(1, "FAIL: %s (line %d)\n", msg, __LINE__); \
  thread_exit(); \
} while (0)

// ---------- Test 1: clone() error paths ----------
static void
test_clone_errors(void)
{
  int rc;

  rc = clone(0, MIN_STACK);
  ASSERT_EQ(rc, -1, "clone(NULL, sz) should fail");

  void *tiny = malloc(128);
  rc = clone(tiny, 128);
  ASSERT_EQ(rc, -1, "clone(tiny stack) should fail");
  free(tiny);

  void *ok = malloc(MIN_STACK);
  rc = clone((char*)ok + 1, MIN_STACK - 1);
  ASSERT_EQ(rc, -1, "clone(bad alignment/size) should fail");
  free(ok);

  PASS("clone() invalid-argument errors");
}

// ---------- Test 2: shared address space (memory) ----------
static _Atomic int shared_counter = 0;

static void *
child_inc(void *arg)
{
  (void)arg;
  for (int i = 0; i < 10000; i++) {
    atomic_fetch_add_explicit(&shared_counter, 1, memory_order_seq_cst);
  }
  thread_exit();
  return 0;
}

static void
test_shared_address_space(void)
{
  shared_counter = 0;

  int pid = thread_create(child_inc, 0);
  ASSERT_TRUE(pid > 0, "thread_create returned valid pid");

  for (int i = 0; i < 10000; i++) {
    atomic_fetch_add_explicit(&shared_counter, 1, memory_order_seq_cst);
  }

  int w = thread_wait(pid);
  ASSERT_EQ(w, pid, "thread_wait returns joined pid");
  ASSERT_EQ(shared_counter, 20000, "parent and child see the same memory");
  PASS("shared address space (atomic increments)");
}

// ---------- Test 3: shared FD table & offsets/close ----------
static int g_fd = -1;
static _Atomic int g_ready = 0;
static void *g_chan_ready = &g_ready;

static void *
writer_thread(void *arg)
{
  (void)arg;
  int n = write(g_fd, "A", 1);
  if (n != 1) {
    CHILD_FAIL("child write A");
  }
  atomic_store_explicit(&g_ready, 1, memory_order_seq_cst);
  unpark(g_chan_ready);
  thread_exit();
  return 0;
}

static int g_fd2 = -1;
static void *
closer_thread(void *arg)
{
  (void)arg;
  close(g_fd2);
  thread_exit();
  return 0;
}

static void
test_shared_fd_table(void)
{
  unlink("tl_fd.txt");
  g_fd = open("tl_fd.txt", O_CREATE | O_RDWR);
  ASSERT_TRUE(g_fd >= 0, "open tl_fd.txt");

  g_ready = 0;
  g_chan_ready = &g_ready;

  int pid = thread_create(writer_thread, 0);
  ASSERT_TRUE(pid > 0, "thread_create writer");

  setpark(g_chan_ready);
  if (atomic_load_explicit(&g_ready, memory_order_seq_cst) == 0) {
    park(g_chan_ready);
  }

  int n = write(g_fd, "B", 1);
  ASSERT_EQ(n, 1, "parent write B");

  int w = thread_wait(pid);
  ASSERT_EQ(w, pid, "thread_wait writer");

  close(g_fd);
  g_fd = open("tl_fd.txt", 0);
  ASSERT_TRUE(g_fd >= 0, "reopen for read");
  char buf[3] = {0};
  n = read(g_fd, buf, 2);
  ASSERT_EQ(n, 2, "read 2 bytes");
  ASSERT_TRUE(buf[0]=='A' && buf[1]=='B', "shared FD offsets produce 'AB'");
  close(g_fd);

  unlink("tl_fd2.txt");
  g_fd2 = open("tl_fd2.txt", O_CREATE | O_RDWR);
  ASSERT_TRUE(g_fd2 >= 0, "open tl_fd2.txt");
  int pid2 = thread_create(closer_thread, 0);
  ASSERT_TRUE(pid2 > 0, "thread_create closer");
  thread_wait(pid2);

  n = write(g_fd2, "X", 1);
  ASSERT_EQ(n, -1, "write must fail after peer closes shared fd");
  close(g_fd2);

  PASS("shared FD table: offsets & close visibility");
}

// ---------- Test 4: shared CWD via child chdir() ----------
static _Atomic int g_in_dir = 0;
static void *g_chan_cwd = &g_in_dir;

static void *
changer_thread(void *arg)
{
  (void)arg;
  if (chdir("tdir") < 0) {
    CHILD_FAIL("child chdir tdir");
  }
  atomic_store_explicit(&g_in_dir, 1, memory_order_seq_cst);
  unpark(g_chan_cwd);
  thread_exit();
  return 0;
}

static void
test_shared_cwd(void)
{
  mkdir("tdir");

  g_in_dir = 0;
  g_chan_cwd = &g_in_dir;

  int pid = thread_create(changer_thread, 0);
  ASSERT_TRUE(pid > 0, "thread_create changer");

  setpark(g_chan_cwd);
  if (atomic_load_explicit(&g_in_dir, memory_order_seq_cst) == 0) {
    park(g_chan_cwd);
  }

  int fd = open("marker", O_CREATE | O_RDWR);
  ASSERT_TRUE(fd >= 0, "parent created relative file after child chdir");
  close(fd);

  chdir("/");
  fd = open("tdir/marker", 0);
  ASSERT_TRUE(fd >= 0, "marker is under shared CWD (tdir)");
  close(fd);

  thread_wait(pid);
  PASS("shared CWD");
}

// ---------- Test 5: waitpid() semantics & errors ----------
static void *
noop_thread(void *a)
{
  (void)a;
  thread_exit();
  return 0;
}

static void
test_waitpid(void)
{
  int pid = thread_create(noop_thread, 0);
  ASSERT_TRUE(pid > 0, "thread_create noop");
  int r = waitpid(pid);
  ASSERT_EQ(r, 0, "waitpid returns 0 on success");

  r = waitpid(999999);
  ASSERT_EQ(r, -1, "waitpid(bogus) returns -1");
  PASS("waitpid()");
}

// ---------- Test 6: thread_create/thread_wait wrappers ----------
static _Atomic int g_seen = 0;

static void *
work_thread(void *arg)
{
  int v = (int)(uint)arg;
  atomic_store_explicit(&g_seen, v, memory_order_seq_cst);
  thread_exit();
  return 0;
}

static void
test_thread_wrappers(void)
{
  g_seen = 0;
  int pid = thread_create(work_thread, (void*)(uint)1234);
  ASSERT_TRUE(pid > 0, "thread_create");
  int joined = thread_wait(pid);
  ASSERT_EQ(joined, pid, "thread_wait returns same pid");
  ASSERT_EQ(atomic_load_explicit(&g_seen, memory_order_seq_cst), 1234, "start_routine(arg) works");
  PASS("thread_create/thread_wait");
}

// ---------- Test 7: spinlock correctness ----------
static struct spinlock g_s;
static _Atomic int spin_ctr = 0;

static void *
spin_worker(void *arg)
{
  int iters = (int)(uint)arg;
  for (int i = 0; i < iters; i++) {
    spinlock_acquire(&g_s);
    int tmp = spin_ctr;
    tmp++;
    spin_ctr = tmp;
    spinlock_release(&g_s);
  }
  thread_exit();
  return 0;
}

static void
test_spinlock(void)
{
  ASSERT_EQ(spinlock_init(&g_s), 0, "spinlock_init");
  spin_ctr = 0;

  int iters = 5000;
  int p1 = thread_create(spin_worker, (void*)(uint)iters);
  int p2 = thread_create(spin_worker, (void*)(uint)iters);
  ASSERT_TRUE(p1 > 0 && p2 > 0, "create spin workers");
  thread_wait(p1);
  thread_wait(p2);

  ASSERT_EQ(spin_ctr, iters*2, "spinlock protects critical section");
  PASS("spinlock acquire/release");
}

// ---------- Test 8: mutex + (set)park/unpark ----------
static struct mutex g_m;
static _Atomic int gate = 0;

static void *
mutex_waiter(void *arg)
{
  (void)arg;
  if (mutex_acquire(&g_m) != 0) CHILD_FAIL("mutex_acquire waiter");
  void *chan = &gate;
  for (;;) {
    int g = atomic_load_explicit(&gate, memory_order_seq_cst);
    if (g == 1) break;
    setpark(chan);
    if (atomic_load_explicit(&gate, memory_order_seq_cst) != 1) {
      park(chan);
    }
  }
  if (mutex_release(&g_m) != 0) CHILD_FAIL("mutex_release waiter");
  thread_exit();
  return 0;
}

static void
test_mutex_park(void)
{
  ASSERT_EQ(mutex_init(&g_m), 0, "mutex_init");
  ASSERT_EQ(mutex_acquire(&g_m), 0, "mutex_acquire main");

  int pw = thread_create(mutex_waiter, 0);
  ASSERT_TRUE(pw > 0, "spawn waiter");

  sleep(1); // give waiter time to sleep

  atomic_store_explicit(&gate, 1, memory_order_seq_cst);
  unpark(&gate);

  ASSERT_EQ(mutex_release(&g_m), 0, "mutex_release main");
  thread_wait(pw);
  PASS("mutex with (set)park/unpark (lost-wakeup safe)");
}

// ---------- Test 9: condvar (producer-consumer 1x) ----------
static struct condvar g_cv;
static struct mutex g_cm;
static _Atomic int slot_full = 0;
static int mailbox = 0;

static void *
producer(void *arg)
{
  int value = (int)(uint)arg;

  if (mutex_acquire(&g_cm) != 0) CHILD_FAIL("cond producer lock");
  while (atomic_load_explicit(&slot_full, memory_order_seq_cst)) {
    cond_wait(&g_cv, &g_cm);
  }
  mailbox = value;
  atomic_store_explicit(&slot_full, 1, memory_order_seq_cst);
  cond_signal(&g_cv);
  if (mutex_release(&g_cm) != 0) CHILD_FAIL("cond producer unlock");
  thread_exit();
  return 0;
}

static void *
consumer(void *arg)
{
  (void)arg;
  if (mutex_acquire(&g_cm) != 0) CHILD_FAIL("cond consumer lock");
  while (!atomic_load_explicit(&slot_full, memory_order_seq_cst)) {
    cond_wait(&g_cv, &g_cm);
  }
  int got = mailbox;
  atomic_store_explicit(&slot_full, 0, memory_order_seq_cst);
  cond_signal(&g_cv);
  if (mutex_release(&g_cm) != 0) CHILD_FAIL("cond consumer unlock");

  mailbox = got; // visible to parent
  thread_exit();
  return 0;
}

static void
test_condvar(void)
{
  ASSERT_EQ(mutex_init(&g_cm), 0, "mutex_init cond");
  ASSERT_EQ(cond_init(&g_cv), 0, "cond_init");
  atomic_store_explicit(&slot_full, 0, memory_order_seq_cst);
  mailbox = 0;

  int pc = thread_create(consumer, 0);
  ASSERT_TRUE(pc > 0, "spawn consumer");
  int pp = thread_create(producer, (void*)(uint)42);
  ASSERT_TRUE(pp > 0, "spawn producer");

  thread_wait(pp);
  thread_wait(pc);

  ASSERT_EQ(mailbox, 42, "condvar delivers value");
  PASS("condvar producer/consumer(1)");
}

int
main(int argc, char **argv)
{
  (void)argc; (void)argv;

  printf(1, "== Thread Library Error/Security Tests ==\n");

  test_clone_errors();
  test_shared_address_space();
  test_shared_fd_table();
  test_shared_cwd();
  test_waitpid();
  test_thread_wrappers();
  test_spinlock();
  test_mutex_park();
  test_condvar();

  printf(1, "ALL TESTS PASSED\n");
  exit();
}
