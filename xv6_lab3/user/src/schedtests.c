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

static int try_setscheduler(int pid, int policy, int prio) {
  return setscheduler(pid, policy, prio);
}

static void align_to_next_tick(void) {
  uint t0 = uptime();
  while (uptime() == t0) { /* spin */ }
}

// ------------- Baseline tests -------------
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
  sleep(2);
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

// ------------- Edge cases from prior suite -------------
static void test_wakeup_preempt_same_tick(void) {
  printf(1, "\n[EC wakeup_preempt] Wake higher-prio RR via pipe; should preempt immediately.\n");
  int fds[2];
  if (pipe(fds) < 0) { printf(1, "pipe failed\n"); exit(); }

  int high = fork();
  if (high == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 50, "RR-high(wakeup)");
    char buf;
    read(fds[0], &buf, 1);
    burn_ticks(8, "RR-high(woke)");
    exit();
  }

  int low = fork();
  if (low == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 1, "RR-low");
    burn_ticks(20, "RR-low");
    exit();
  }

  align_to_next_tick();
  uint target = uptime() + 3;
  while (uptime() < target) { /* wait */ }
  printf(1, "[EC wakeup_preempt] parent writing to pipe at tick %d\n", uptime());
  write(fds[1], "X", 1);

  close(fds[0]); close(fds[1]);
  wait(); wait();
  printf(1, "[EC wakeup_preempt] done.\n");
}

static void test_fifo_preempt_fifo_prio(void) {
  printf(1, "\n[EC fifo>fifo] High-prio FIFO spawns while low-prio FIFO is running; expect immediate preempt.\n");
  int low = fork();
  if (low == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 1, "FIFO-low");
    burn_ticks(20, "FIFO-low");
    exit();
  }
  sleep(2);
  int high = fork();
  if (high == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 10, "FIFO-high");
    burn_ticks(10, "FIFO-high");
    exit();
  }
  wait(); wait();
  printf(1, "[EC fifo>fifo] done.\n");
}

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

static void test_priority_change_preempts_peer(void) {
  printf(1, "\n[EC bump_peer] A=FIFO p1 running; B=RR p1 RUNNABLE; bump B->p100, expect immediate preempt of A.\n");

  int a = fork();
  if (a == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 1, "FIFO-A(p1)");
    burn_ticks(30, "FIFO-A(p1)");
    exit();
  }

  int b = fork();
  if (b == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 1, "RR-B(p1->p100)");
    burn_ticks(12, "RR-B(after bump)");
    exit();
  }

  align_to_next_tick();
  uint target = uptime() + 2;
  while (uptime() < target) { /* wait */ }

  printf(1, "[EC bump_peer] parent bumping B at tick %d\n", uptime());
  set_sched_or_die(b, SCHED_RR, 100, "RR-B bump");

  wait(); wait();
  printf(1, "[EC bump_peer] done.\n");
}

static void test_defaults_behavior(void) {
  printf(1, "\n[DEF defaults] Unconfigured child should behave as RR p=0 and interleave with RR(0).\n");
  int d1 = fork();
  if (d1 == 0) {
    // No setscheduler: should be RR, priority 0 by default
    burn_ticks(12, "default-RR");
    exit();
  }
  int d2 = fork();
  if (d2 == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 0, "explicit-RR");
    burn_ticks(12, "explicit-RR");
    exit();
  }
  wait(); wait();
  printf(1, "[DEF defaults] done.\n");
}

// Creation vs arrival order for RR: A created before B, but A sleeps longer.
// Expect: at equal (policy,prio) the scheduler order is by creation, not arrival.
static void test_rr_creation_vs_arrival(void) {
  printf(1, "\n[ORDER rr_created] RR p=2: A created before B; B wakes first; expect A before B when both runnable.\n");
  int a = fork();
  if (a == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 2, "RR-A");
    sleep(8); // wake after B
    burn_ticks(8, "RR-A");
    exit();
  }
  int b = fork();
  if (b == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 2, "RR-B");
    sleep(3); // wakes earlier
    burn_ticks(8, "RR-B");
    exit();
  }
  wait(); wait();
  printf(1, "[ORDER rr_created] done.\n");
}

// Creation vs arrival order for FIFO: similar expectation-creation order defines FIFO queue.
static void test_fifo_creation_vs_arrival(void) {
  printf(1, "\n[ORDER fifo_created] FIFO p=2: A created before B; B wakes first; expect A runs first when both runnable.\n");
  int a = fork();
  if (a == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 2, "FIFO-A");
    sleep(8);
    burn_ticks(6, "FIFO-A");
    exit();
  }
  int b = fork();
  if (b == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 2, "FIFO-B");
    sleep(3);
    burn_ticks(6, "FIFO-B");
    exit();
  }
  wait(); wait();
  printf(1, "[ORDER fifo_created] done.\n");
}

// Only self or direct children are allowed. Also negative priority should be rejected.
static void test_syscall_acl_self_and_child(void) {
  printf(1, "\n[ACL self_child] Self and direct child should be allowed.\n");
  // Self change
  int r = try_setscheduler(getpid(), SCHED_RR, 0);
  printf(1, "  self setscheduler -> %s\n", r < 0 ? "FAIL (unexpected)" : "OK");

  // Child change
  int c = fork();
  if (c == 0) {
    // Child sets itself
    int rr = try_setscheduler(getpid(), SCHED_RR, 1);
    printf(1, "  child self setscheduler -> %s\n", rr < 0 ? "FAIL" : "OK");
    exit();
  }
  wait();
  printf(1, "[ACL self_child] done.\n");
}

static void test_syscall_acl_forbidden_other(void) {
  printf(1, "\n[ACL forbidden] Sibling and grandchild modifications should fail.\n");
  int c1 = fork();
  if (c1 == 0) {
    sleep(20); // keep c1 alive while c2 tries
    exit();
  }
  int c2 = fork();
  if (c2 == 0) {
    // This child tries to change its sibling (not allowed)
    int rc = try_setscheduler(c1, SCHED_FIFO, 5);
    printf(1, "  sibling setscheduler -> %s\n", rc < 0 ? "OK (rejected)" : "FAIL (should reject)");

    // Create a grandchild, then try to modify it from the parent later (parent->grandchild should be forbidden)
    int gc = fork();
    if (gc == 0) {
      sleep(50); // stay alive
      exit();
    }
    sleep(5); // allow parent to attempt later

    exit();
  }
  // Parent tries to change grandchild (not a direct child if performed after c2 exits or if we target gc)
  sleep(10);
  // We don't know the grandchild pid here (lacking IPC).
  int rc2 = try_setscheduler(c2 + 10000, SCHED_RR, 0);
  printf(1, "  non-child pid setscheduler -> %s\n", rc2 < 0 ? "OK (rejected)" : "FAIL (should reject)");

  wait(); wait();
  printf(1, "[ACL forbidden] done.\n");
}

static void test_negative_priority_rejected(void) {
  printf(1, "\n[ACL negative] Negative priority should be rejected.\n");
  int rc = try_setscheduler(getpid(), SCHED_RR, -1);
  printf(1, "  setscheduler(prio=-1) -> %s\n", rc < 0 ? "OK (rejected)" : "FAIL (accepted)");
  printf(1, "[ACL negative] done.\n");
}

static void test_policy_change_runtime(void) {
  printf(1, "\n[POL switch] Running RR(1) switched to FIFO(1); RR peer should not run until switcher blocks.\n");
  int runner = fork();
  if (runner == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 1, "runner");
    burn_ticks(6, "runner RR(1)");
    // parent will switch our policy while we run
    burn_ticks(20, "runner after switch");
    exit();
  }
  sleep(2);
  int rrpeer = fork();
  if (rrpeer == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 1, "peer");
    burn_ticks(10, "RR peer");
    exit();
  }
  // Give runner a moment, then flip it to FIFO at same priority.
  align_to_next_tick();
  printf(1, "  parent switching runner to FIFO(1) at tick %d\n", uptime());
  set_sched_or_die(runner, SCHED_FIFO, 1, "runner->FIFO");

  wait(); wait();
  printf(1, "[POL switch] done.\n");
}

static void test_rr_quantum_sanity(void) {
  printf(1, "\n[RR quantum] RR(0) x3 should rotate roughly 1 tick each.\n");
  for (int i = 0; i < 3; i++) {
    int p = fork();
    if (p == 0) {
      set_sched_or_die(getpid(), SCHED_RR, 0, "RR-q");
      burn_ticks(9, "RR-q");
      exit();
    }
  }
  wait(); wait(); wait();
  printf(1, "[RR quantum] done.\n");
}

static void test_starvation_demo(void) {
  printf(1, "\n[STARVE demo] FIFO(100) long-run should delay RR(0). Expected by spec.\n");
  int f = fork();
  if (f == 0) {
    set_sched_or_die(getpid(), SCHED_FIFO, 100, "FIFO-king");
    burn_ticks(25, "FIFO-king");
    exit();
  }
  int r = fork();
  if (r == 0) {
    set_sched_or_die(getpid(), SCHED_RR, 0, "RR-peasant");
    burn_ticks(10, "RR-peasant");
    exit();
  }
  wait(); wait();
  printf(1, "[STARVE demo] done.\n");
}

static void usage(void) {
  printf(1, "Usage: sched_test [rr | fifo_vs_rr | prio_preempt | fifo_eq |\n");
  printf(1, "                    wakeup_preempt | fifo_preempt_fifo | fifo_preempts_rr_sameprio | bump_peer |\n");
  printf(1, "                    defaults | rr_created | fifo_created | acl_self_child | acl_forbidden | acl_negative |\n");
  printf(1, "                    pol_switch | rr_quantum | starve | all]\n");
}

int main(int argc, char **argv) {
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
  } else if (strcmp(argv[1], "defaults") == 0) {
    test_defaults_behavior();
  } else if (strcmp(argv[1], "rr_created") == 0) {
    test_rr_creation_vs_arrival();
  } else if (strcmp(argv[1], "fifo_created") == 0) {
    test_fifo_creation_vs_arrival();
  } else if (strcmp(argv[1], "acl_self_child") == 0) {
    test_syscall_acl_self_and_child();
  } else if (strcmp(argv[1], "acl_forbidden") == 0) {
    test_syscall_acl_forbidden_other();
  } else if (strcmp(argv[1], "acl_negative") == 0) {
    test_negative_priority_rejected();
  } else if (strcmp(argv[1], "pol_switch") == 0) {
    test_policy_change_runtime();
  } else if (strcmp(argv[1], "rr_quantum") == 0) {
    test_rr_quantum_sanity();
  } else if (strcmp(argv[1], "starve") == 0) {
    test_starvation_demo();
  } else if (strcmp(argv[1], "all") == 0) {
    // Baselines
    test_rr();
    test_fifo_vs_rr();
    test_prio_preempt();
    test_fifo_equal_priority();
    // Edge cases from earlier suite
    test_wakeup_preempt_same_tick();
    test_fifo_preempt_fifo_prio();
    test_fifo_arrival_preempts_rr_sameprio();
    test_priority_change_preempts_peer();
    // New coverage
    test_defaults_behavior();
    test_rr_creation_vs_arrival();
    test_fifo_creation_vs_arrival();
    test_syscall_acl_self_and_child();
    test_syscall_acl_forbidden_other();
    test_negative_priority_rejected();
    test_policy_change_runtime();
    test_rr_quantum_sanity();
    test_starvation_demo();
  } else {
    usage();
  }

  printf(1, "sched_test: done at tick=%d\n", uptime());
  exit();
}
