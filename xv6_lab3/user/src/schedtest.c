// user/sched_test.c
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"

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

static void set_sched_or_die(int pid, int policy, int prio, const char *who) {
  if (setscheduler(pid, policy, prio) < 0) {
    printf(1, "ERROR: setscheduler(pid=%d, policy=%d, prio=%d) for %s\n",
           pid, policy, prio, who);
    exit();
  }
}

// ---------- BASELINE TESTS ----------
static void test_rr(void) {
  printf(1, "\n[TEST rr] 2 RR children, same priority=0, expect interleaving.\n");
  int p1 = fork();
  if (p1 == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 0, "RR-1");
    burn_ticks(15, "RR-1");
    exit();
  }
  int p2 = fork();
  if (p2 == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 0, "RR-2");
    burn_ticks(15, "RR-2");
    exit();
  }
  wait(); wait();
  printf(1, "[TEST rr] done.\n");
}

static void test_fifo_vs_rr(void) {
  printf(1, "\n[TEST fifo_vs_rr] FIFO(0) vs RR(100). Expect FIFO runs to completion first.\n");
  int pf = fork();
  if (pf == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 0, "FIFO");
    burn_ticks(12, "FIFO");
    exit();
  }
  int pr = fork();
  if (pr == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 100, "RR");
    burn_ticks(12, "RR");
    exit();
  }
  wait(); wait();
  printf(1, "[TEST fifo_vs_rr] done.\n");
}

static void test_prio_preempt(void) {
  printf(1, "\n[TEST prio_preempt] RR-low starts; RR-high appears and should preempt immediately.\n");
  int low = fork();
  if (low == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 5, "RR-low");
    burn_ticks(20, "RR-low");
    exit();
  }
  sleep(2); // give low a head start
  int high = fork();
  if (high == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 10, "RR-high");
    burn_ticks(12, "RR-high");
    exit();
  }
  wait(); wait();
  printf(1, "[TEST prio_preempt] done.\n");
}

static void test_fifo_equal_priority(void) {
  printf(1, "\n[TEST fifo_eq] Two FIFO prio=1. A runs until it blocks; B runs; A resumes.\n");
  int a = fork();
  if (a == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 1, "FIFO-A");
    burn_ticks(6, "FIFO-A (phase1)");
    printf(1, "FIFO-A blocking via sleep...\n");
    sleep(5);
    burn_ticks(6, "FIFO-A (phase2)");
    exit();
  }
  int b = fork();
  if (b == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 1, "FIFO-B");
    burn_ticks(10, "FIFO-B");
    exit();
  }
  wait(); wait();
  printf(1, "[TEST fifo_eq] done.\n");
}

// ---------- EDGE CASES ----------

// EC1: Immediate preempt on wakeup (same-tick) via pipe.
// Setup: High-prio RR child blocks on read(pipe). Low-prio RR is running.
// Parent writes into pipe at a known tick to wake high-prio.
// Expectation: After the write, the next print should be from the high-prio child,
// not the low-prio, demonstrating "immediate" preemption on wakeup.
static void test_wakeup_preempt_same_tick(void) {
  printf(1, "\n[EC wakeup_preempt] Wake higher-prio RR via pipe; should preempt immediately.\n");
  int fds[2];
  if (pipe(fds) < 0) { printf(1, "pipe failed\n"); exit(); }

  int high = fork();
  if (high == 0) {
    // High priority RR waits for data; becomes RUNNABLE exactly when parent writes.
    set_sched_or_die(getpid(), SCHED_RR, 50, "RR-high(wakeup)");
    char buf;
    // Block here:
    read(fds[0], &buf, 1);
    // On wake, we should preempt low immediately and start printing.
    burn_ticks(8, "RR-high(woke)");
    exit();
  }

  int low = fork();
  if (low == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 1, "RR-low");
    // Busy while parent times the write.
    burn_ticks(20, "RR-low");
    exit();
  }

  // Parent: align the write roughly mid-run of low.
  // Aim to write at the boundary between ticks to stress "same tick".
  uint t0 = uptime();
  while (uptime() == t0) {  }
  uint target = uptime() + 3;   // pick a few ticks ahead for stability
  while (uptime() < target) { }
  printf(1, "[EC wakeup_preempt] parent writing to pipe at tick %d\n", uptime());
  write(fds[1], "X", 1); // should make 'high' RUNNABLE and trigger preempt

  close(fds[0]); close(fds[1]);
  wait(); wait();
  printf(1, "[EC wakeup_preempt] done.\n");
}

// EC2: FIFO higher priority arrives later and must preempt running FIFO lower prio.
static void test_fifo_preempt_fifo_prio(void) {
  printf(1, "\n[EC fifo>fifo] High-prio FIFO spawns while low-prio FIFO is running; expect immediate preempt.\n");
  int low = fork();
  if (low == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 1, "FIFO-low");
    burn_ticks(20, "FIFO-low");
    exit();
  }
  sleep(2); // let low start and be clearly RUNNING
  int high = fork();
  if (high == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 10, "FIFO-high");
    burn_ticks(10, "FIFO-high");
    exit();
  }
  wait(); wait();
  printf(1, "[EC fifo>fifo] done.\n");
}

// EC3: FIFO arrival preempts RR at same priority (policy dominance).
static void test_fifo_arrival_preempts_rr_sameprio(void) {
  printf(1, "\n[EC fifo_vs_rr_sameprio] RR prio=3 running; FIFO prio=3 arrives and should preempt immediately.\n");
  int rr = fork();
  if (rr == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 3, "RR(3)");
    burn_ticks(20, "RR(3)");
    exit();
  }
  sleep(2);
  int fifo = fork();
  if (fifo == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 3, "FIFO(3)");
    burn_ticks(8, "FIFO(3)");
    exit();
  }
  wait(); wait();
  printf(1, "[EC fifo_vs_rr_sameprio] done.\n");
}

// EC4: Raising a RUNNABLE peer's priority should preempt the current runner.
// Setup: Two RR children. Child A starts running at prio=1. Child B is RUNNABLE at prio=1.
// Parent bumps B's priority to 100 while A is running.
// Expectation: Kernel must notice a better RUNNABLE candidate and preempt A immediately.
static void test_priority_change_preempts_peer(void) {
  printf(1, "\n[EC bump_peer] A=FIFO p1 running; B=RR p1 RUNNABLE; bump B->p100, expect immediate preempt of A.\n");

  int a = fork();
  if (a == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 1, "FIFO-A(p1)");
    // Long run so parent can bump during A's execution.
    burn_ticks(30, "FIFO-A(p1)");
    exit();
  }

  // Spawn B; it'll be RUNNABLE but won't run while A (FIFO) is active.
  int b = fork();
  if (b == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 1, "RR-B(p1->p100)");
    // Once bumped, we should take over and print densely.
    burn_ticks(12, "RR-B(after bump)");
    exit();
  }

  // Give A a head start and ensure we're in A's run window.
  // Align to a tick boundary, then wait 2 ticks so A is definitely RUNNING.
  uint t0 = uptime();
  while (uptime() == t0) { /* spin to next tick */ }
  uint target = uptime() + 2;
  while (uptime() < target) { /* wait */ }

  printf(1, "[EC bump_peer] parent bumping B at tick %d\n", uptime());
  set_sched_or_die(b, SCHED_RR, 100, "RR-B bump");

  // After the bump, the very next printed line should be from B (RR-B after bump).
  wait(); wait();
  printf(1, "[EC bump_peer] done.\n");
}

static void usage(void) {
  printf(1, "Usage: sched_test [rr | fifo_vs_rr | prio_preempt | fifo_eq |\n");
  printf(1, "                    wakeup_preempt | fifo_preempt_fifo | fifo_preempts_rr_sameprio | bump_peer | all]\n");
}

int
main(int argc, char **argv)
{
  printf(1, "sched_test: pid=%d, start tick=%d\n", getpid(), uptime());

  if (argc < 2) { usage(); exit(); }

  if (strcmp(argv[1], "rr") == 0) {
    test_rr();
  } else if (strcmp(argv[1], "fifo_vs_rr") == 0) {
    test_fifo_vs_rr();
  } else if (strcmp(argv[1], "prio_preempt") == 0) {
    test_prio_preempt();
  } else if (strcmp(argv[1], "fifo_eq") == 0) {
    test_fifo_equal_priority();
  } else if (strcmp(argv[1], "wakeup_preempt") == 0) {
    test_wakeup_preempt_same_tick();
  } else if (strcmp(argv[1], "fifo_preempt_fifo") == 0) {
    test_fifo_preempt_fifo_prio();
  } else if (strcmp(argv[1], "fifo_preempts_rr_sameprio") == 0) {
    test_fifo_arrival_preempts_rr_sameprio();
  } else if (strcmp(argv[1], "bump_peer") == 0) {
    test_priority_change_preempts_peer();
  } else if (strcmp(argv[1], "all") == 0) {
    // Baselines first
    test_rr();
    test_fifo_vs_rr();
    test_prio_preempt();
    test_fifo_equal_priority();
    // Edge cases
    test_wakeup_preempt_same_tick();
    test_fifo_preempt_fifo_prio();
    test_fifo_arrival_preempts_rr_sameprio();
    test_priority_change_preempts_peer();
  } else {
    usage();
  }

  printf(1, "sched_test: done at tick=%d\n", uptime());
  exit();
}
