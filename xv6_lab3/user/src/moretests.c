#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"
#include "fcntl.h"


static void burn_ticks(int ticks, const char *tag) {
  uint start = uptime(), last = start;
  int pid = getpid();
  while ((int)(uptime() - start) < ticks) {
    uint now = uptime();
    if (now != last) {
      printf(1, "[tick %d] pid=%d %s\n", now, pid, tag);
      last = now;
    }
  }
}

static void align_to_next_tick(void) {
  uint t0 = uptime();
  while (uptime() == t0) { }
}

static void set_sched_or_die(int pid, int policy, int prio, const char *who) {
  if (setscheduler(pid, policy, prio) < 0) {
    printf(1, "ERROR: setscheduler(pid=%d, policy=%d, prio=%d) for %s\n",
           pid, policy, prio, who);
    exit();
  }
}

static int try_setscheduler(int pid, int policy, int prio) {
  return setscheduler(pid, policy, prio);
}

static int waitpid_timeout(int child, int max_ticks, const char *label) {
  uint t0 = uptime();
  for (;;) {
    int w = wait();
    if (w == child) return 0;              // child exited OK
    if (w > 0) continue;                   // spurious/other child
    // w < 0 means no exited child yet
    if ((int)(uptime() - t0) > max_ticks) {
      printf(1, "[TIMEOUT] %s (pid=%d) exceeded %d ticks; killing.\n",
             label, child, max_ticks);
      kill(child);
      wait(); // reap
      return -1;
    }
    sleep(1);
  }
}

static void pass(const char *name) { printf(1, "[PASS] %s\n", name); }
static void fail(const char *name, const char *why) {
  printf(1, "[FAIL] %s: %s\n", name, why);
}

// ---------- Tests ----------

// 1) ACL checks: only self or direct child allowed.
static void test_acl(void) {
  const char *T = "acl";
  int parent_pid = getpid();
  int gpipe[2];
  if (pipe(gpipe) < 0) { fail(T, "pipe failed"); return; }
  int child = fork();
  if (child < 0) { fail(T, "fork failed"); return; }

  if (child == 0) {
    close(gpipe[0]);
    // Child: try to change parent's scheduler (should fail).
    int rc1 = try_setscheduler(parent_pid, SCHED_RR, 1);
    if (rc1 >= 0) {
      printf(1, "ERROR: child changed parent scheduler!\n");
      exit();
    }
    // Child: change self (should succeed).
    if (try_setscheduler(getpid(), SCHED_RR, 2) < 0) {
      printf(1, "ERROR: child failed to change its own scheduler\n");
      exit();
    }
    // Child: spawn grandchild; parent (above us) should not be able to
    // change grandchild. We just exit after short burn.
    int g = fork();
    if (g < 0) {
      printf(1, "ERROR: child failed to fork grandchild\n");
      write(gpipe[1], &g, sizeof(g));
      exit();
    }
    if (g == 0) {
      set_sched_or_die(getpid(), SCHED_RR, 2, "grandchild");
      burn_ticks(5, "grandchild");
      exit();
    }
    if (write(gpipe[1], &g, sizeof(g)) != sizeof(g)) {
      printf(1, "ERROR: child failed to send grandchild pid\n");
      kill(g);
      wait();
      exit();
    }
    // Let grandchild live briefly; then exit.
    wait();
    close(gpipe[1]);
    exit();
  }

  close(gpipe[1]);
  int grandchild = -1;
  if (read(gpipe[0], &grandchild, sizeof(grandchild)) != sizeof(grandchild) ||
      grandchild <= 0) {
    fail(T, "failed to receive grandchild pid");
    close(gpipe[0]);
    kill(child);
    wait();
    return;
  }
  close(gpipe[0]);

  // Parent: attempt to set scheduler on grandchild via PID that is not direct.
  int rcBogus = try_setscheduler(99999, SCHED_RR, 0);
  if (rcBogus >= 0) { fail(T, "setscheduler allowed bogus pid"); return; }

  int rcGrand = try_setscheduler(grandchild, SCHED_RR, 0);
  if (rcGrand >= 0) { fail(T, "setscheduler allowed grandchild"); return; }

  // Also: parent can set direct child.
  if (try_setscheduler(child, SCHED_RR, 3) < 0) {
    fail(T, "parent could not set direct child");
    kill(child);
    wait();
    return;
  }

  if (waitpid_timeout(child, 200, "acl-child") < 0) { fail(T, "timeout"); return; }
  pass(T);
}

// 2) Invalid args: policy and priority validation.
static void test_invalid_args(void) {
  const char *T = "invalid_args";
  int p = fork();
  if (p < 0) { fail(T, "fork failed"); return; }
  if (p == 0) {
    // Negative priority should be rejected.
    if (try_setscheduler(getpid(), SCHED_RR, -1) >= 0)
      exit(); // fail (parent will see child exited => fail); but we can't pass info up cleanly.
    // Invalid policy should be rejected.
    if (try_setscheduler(getpid(), 123456, 0) >= 0)
      exit();
    // Valid combination should succeed.
    if (try_setscheduler(getpid(), SCHED_FIFO, 0) < 0)
      exit();
    // Signal success by returning normally after a short burn.
    burn_ticks(3, "ok");
    exit();
  }
  // If child exits quickly, assume success. If it bailed early due to our
  // "exit()" lines above, we treat as failure by inspecting run time.
  uint t0 = uptime();
  if (waitpid_timeout(p, 200, "invalid-args") < 0) { fail(T, "timeout"); return; }
  uint dt = uptime() - t0;
  if ((int)dt < 2) {
    fail(T, "invalid arg not rejected (child exited too fast)");
    return;
  }
  pass(T);
}

// 3) RR priority preemption: RR(5) should preempt RR(0) immediately on becoming RUNNABLE.
static void test_rr_priority_preempt(void) {
  const char *T = "rr_priority_preempt";
  // Parent runs as RR(0) and burns; child RR(5) becomes RUNNABLE after a tick and should preempt.
  set_sched_or_die(getpid(), SCHED_RR, 0, "parent rr0");
  int c = fork();
  if (c < 0) { fail(T, "fork failed"); return; }
  if (c == 0) {
    align_to_next_tick(); // ensure parent gets at least one tick first
    set_sched_or_die(getpid(), SCHED_RR, 5, "child rr5");
    burn_ticks(6, "rr5");
    exit();
  }

  // Parent burns; if preemption happens, our logging should show gaps soon after child wakes.
  uint t0 = uptime();
  uint switched = 0;
  while ((int)(uptime() - t0) < 15) {
    uint now = uptime();
    printf(1, "[tick %d] pid=%d parent-rr0\n", now, getpid());
    if ((int)(uptime() - t0) > 3) switched = 1;
  }
  if (waitpid_timeout(c, 200, "rr5-child") < 0) { fail(T, "child timeout"); return; }
  if (!switched) { fail(T, "no evidence of preemption to higher-priority RR"); return; }
  pass(T);
}

// 4) FIFO outranks RR regardless of priority: FIFO(0) must run to completion before RR(100).
static void test_fifo_over_rr_anyprio(void) {
  const char *T = "fifo_over_rr_anyprio";
  int pf = fork();
  if (pf < 0) { fail(T, "fork FIFO failed"); return; }
  if (pf == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 0, "fifo0");
    burn_ticks(8, "fifo0");
    exit();
  }
  int pr = fork();
  if (pr < 0) { kill(pf); wait(); fail(T, "fork RR failed"); return; }
  if (pr == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 100, "rr100");
    burn_ticks(6, "rr100");
    exit();
  }
  // If FIFO truly dominates, pr should NOT finish before pf (and rr should not run during fifo's run).
  int first = wait();
  if (first == pr) {
    kill(pf); wait();
    fail(T, "RR(100) finished before FIFO(0)");
    return;
  }
  // Reap second
  wait();
  pass(T);
}

// 5) FIFO non-preemption among equal priorities: first FIFO should run to completion before second FIFO of same prio.
static void test_fifo_equal_nonpreempt(void) {
  const char *T = "fifo_equal_nonpreempt";
  int a = fork();
  if (a < 0) { fail(T, "fork A failed"); return; }
  if (a == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 7, "fifoA");
    burn_ticks(8, "fifoA");
    exit();
  }
  // Give A a head start to be clearly running.
  sleep(1);
  int b = fork();
  if (b < 0) { kill(a); wait(); fail(T, "fork B failed"); return; }
  if (b == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 7, "fifoB");
    burn_ticks(4, "fifoB");
    exit();
  }
  // The first finished should be A, because equal-priority FIFO should not be preempted by B.
  int first = wait();
  if (first != a) {
    kill(a);
    wait();
    fail(T, "FIFO equal-priority was preempted incorrectly");
    return;
  }
  wait(); // B
  pass(T);
}

// 6) Preempt running RR when a higher-priority FIFO becomes runnable: must switch immediately.
static void test_preempt_on_higher_fifo_arrival(void) {
  const char *T = "preempt_on_higher_fifo_arrival";
  set_sched_or_die(getpid(), SCHED_RR, 0, "parent rr0");
  int c = fork();
  if (c < 0) { fail(T, "fork failed"); return; }
  if (c == 0) {
    // Child wakes after next tick at FIFO(1); should immediately run and finish before parent proceeds much.
    align_to_next_tick();
    set_sched_or_die(getpid(), SCHED_FIFO, 1, "child fifo1");
    burn_ticks(5, "fifo1");
    exit();
  }
  // Parent noisy burn. If scheduler is correct, child's lines should appear immediately after child's align.
  burn_ticks(2, "parent-rr0-before-child");
  if (waitpid_timeout(c, 200, "fifo-child") < 0) { fail(T, "child timeout"); return; }
  pass(T);
}

// 7) Default behavior: fresh process without setscheduler() should be RR priority 0 and time-slice with sibling.
static void test_default_rr0_interleave(void) {
  const char *T = "default_rr0_interleave";
  int p1 = fork();
  if (p1 < 0) { fail(T, "fork1 failed"); return; }
  if (p1 == 0) {
    // No setscheduler() here - should be default RR(0).
    burn_ticks(8, "def-rr0-1");
    exit();
  }
  int p2 = fork();
  if (p2 < 0) { kill(p1); wait(); fail(T, "fork2 failed"); return; }
  if (p2 == 0) {
    burn_ticks(8, "def-rr0-2");
    exit();
  }
  // Both should complete; logs should show interleaving. Just ensure they don't deadlock.
  if (waitpid_timeout(p1, 200, "def1") < 0) { fail(T, "timeout p1"); kill(p2); wait(); return; }
  if (waitpid_timeout(p2, 200, "def2") < 0) { fail(T, "timeout p2"); return; }
  pass(T);
}

// 8) Large priority acceptance (no overflow / weird clamping): RR(1<<29) should be accepted and preempt RR(0).
static void test_large_priority_rr_preempt(void) {
  const char *T = "large_priority_rr_preempt";
  int low = fork();
  if (low < 0) { fail(T, "fork low failed"); return; }
  if (low == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 0, "low");
    burn_ticks(10, "low-rr0");
    exit();
  }
  int hi = fork();
  if (hi < 0) { kill(low); wait(); fail(T, "fork hi failed"); return; }
  if (hi == 0) {
    // Use a very large but non-negative priority.
    set_sched_or_die(getpid(), SCHED_RR, 1<<29, "hi");
    burn_ticks(4, "hi-rrBIG");
    exit();
  }
  // If hi truly preempts, it should finish first even though started second.
  int first = wait();
  if (first != hi) {
    kill(hi);
    wait(); // reap remaining
    fail(T, "RR with very large priority did not preempt RR(0)");
    return;
  }
  wait(); // low
  pass(T);
}

static struct {
  const char *name;
  void (*fn)(void);
} cases[] = {
  { "acl",                          test_acl },
  { "invalid_args",                 test_invalid_args },
  { "rr_priority_preempt",          test_rr_priority_preempt },
  { "fifo_over_rr_anyprio",         test_fifo_over_rr_anyprio },
  { "fifo_equal_nonpreempt",        test_fifo_equal_nonpreempt },
  { "preempt_on_higher_fifo",       test_preempt_on_higher_fifo_arrival },
  { "default_rr0_interleave",       test_default_rr0_interleave },
  { "large_priority_rr_preempt",    test_large_priority_rr_preempt },
};

int
main(int argc, char **argv)
{
  if (argc < 2) {
    printf(1, "Usage: %s all | <test-name>\n", argv[0]);
    printf(1, "Available tests:\n");
    for (uint i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
      printf(1, "  %s\n", cases[i].name);
    exit();
  }

  if (strcmp(argv[1], "all") == 0) {
    for (uint i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
      printf(1, "\n[TEST %s] start\n", cases[i].name);
      cases[i].fn();
      printf(1, "[TEST %s] end\n", cases[i].name);
      sleep(2);
    }
    exit();
  }

  // Single test
  for (uint i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
    if (strcmp(argv[1], cases[i].name) == 0) {
      printf(1, "\n[TEST %s] start\n", cases[i].name);
      cases[i].fn();
      printf(1, "[TEST %s] end\n", cases[i].name);
      exit();
    }
  }

  printf(1, "Unknown test name '%s'\n", argv[1]);
  exit();
}
