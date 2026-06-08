#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define PGSIZE 4096

static int fails = 0;
#define CHECK(msg, cond) do { \
  if (cond) { \
    printf(1, "[OK]   %s\n", msg); \
  } else { \
    printf(1, "[FAIL] %s\n", msg); \
    fails++; \
  } \
} while (0)

/* Allocate a stack region of 'n' bytes, page-aligned, returning base pointer. */
static void* alloc_stack(int n) {
  int cur = (int)sbrk(0);
  int align = ((cur % PGSIZE) == 0) ? 0 : (PGSIZE - (cur % PGSIZE));
  if (align) sbrk(align);
  void *base = sbrk(n);
  if ((int)base == -1) return (void*)0;
  return base;
}

/* Simple sync: parent waits for 1 byte; child writes 1 byte before exit. */
static void sync_parent(int p[2]) {
  char x;
  if (read(p[0], &x, 1) != 1) {
    // best effort; not fatal to tests
  }
  close(p[0]);
  close(p[1]);
}
static void signal_child(int p[2]) {
  write(p[1], "x", 1);
  close(p[1]);
}

/* Test 1: shared address space (global variable). */
static int gval = 1;
static void test_shared_memory(void) {
  void *st = alloc_stack(PGSIZE * 2);
  int pid = clone(st, PGSIZE * 2);
  if (pid < 0) {
    CHECK("clone() for shared memory test", 0);
    return;
  }
  if (pid == 0) {
    // child
    gval += 41; // 1 -> 42, visible to parent if sharing addr space
    exit();
  }
  // parent
  CHECK("waitpid() on child (shared mem test)", waitpid(pid) == 0);
  CHECK("shared address space reflects child's write (gval == 42)", gval == 42);
}

/* Test 2: shared FD table - single file, ordered writes share the same offset. */
static void test_shared_fd_write_offset(void) {
  unlink("tclone_fd.txt");
  int fd = open("tclone_fd.txt", O_CREATE | O_RDWR);
  CHECK("open() test file", fd >= 0);

  int syncp[2];
  pipe(syncp);

  void *st = alloc_stack(PGSIZE * 2);
  int pid = clone(st, PGSIZE * 2);
  if (pid < 0) {
    CHECK("clone() for shared FD write test", 0);
    close(fd);
    return;
  }
  if (pid == 0) {
    // child first write, then signal
    write(fd, "1", 1);
    signal_child(syncp);
    exit();
  }
  sync_parent(syncp);
  write(fd, "2", 1);
  close(fd);

  // Read back and verify order == "12"
  fd = open("tclone_fd.txt", 0);
  char buf[4] = {0};
  int n = read(fd, buf, sizeof(buf));
  close(fd);
  CHECK("shared FD offset results in '12'", n >= 2 && buf[0] == '1' && buf[1] == '2');
}

/* Test 3: closing an FD in one thread affects the other (shared FD table). */
static void test_shared_fd_close_effect(void) {
  unlink("tclone_close.txt");
  int fd = open("tclone_close.txt", O_CREATE | O_RDWR);
  CHECK("open() test file for close", fd >= 0);

  void *st = alloc_stack(PGSIZE * 2);
  int pid = clone(st, PGSIZE * 2);
  if (pid < 0) {
    CHECK("clone() for shared FD close test", 0);
    close(fd);
    return;
  }
  if (pid == 0) {
    // child closes the shared fd; this should invalidate it for the parent too
    close(fd);
    exit();
  }
  CHECK("waitpid() on child (close test)", waitpid(pid) == 0);
  int rc = write(fd, "X", 1);
  CHECK("parent write after child close fails (-1)", rc < 0);
}

/* Test 4: shared current working directory. */
static void test_shared_cwd(void) {
  // Ensure a clean dir and file
  unlink("tclone_dir/touch.txt");
  unlink("touch.txt");
  mkdir("tclone_dir");

  int syncp[2];
  pipe(syncp);

  void *st = alloc_stack(PGSIZE * 2);
  int pid = clone(st, PGSIZE * 2);
  if (pid < 0) {
    CHECK("clone() for cwd test", 0);
    return;
  }
  if (pid == 0) {
    // child changes CWD, then signals
    int rc = chdir("tclone_dir");
    if (rc < 0) {
      // Can't print selectively here; just exit early, test will fail
      exit();
    }
    signal_child(syncp);
    exit();
  }
  // parent waits for child to change CWD; if CWD is shared, our CWD is now tclone_dir
  sync_parent(syncp);
  int fd = open("touch.txt", O_CREATE | O_RDWR);
  CHECK("open('touch.txt') relative to new cwd", fd >= 0);
  if (fd >= 0) close(fd);

  // Verify the file exists at tclone_dir/touch.txt
  struct stat stt;
  int ok = (stat("tclone_dir/touch.txt", &stt) == 0);
  CHECK("relative create landed in shared CWD (tclone_dir/touch.txt exists)", ok);
}

/* Test 5: waitpid() basic behavior and error on non-existent pid. */
static void test_waitpid_basic(void) {
  void *st = alloc_stack(PGSIZE * 2);
  int pid = clone(st, PGSIZE * 2);
  CHECK("clone() for waitpid basic", pid >= 0);
  if (pid == 0) {
    // trivial child
    exit();
  }
  CHECK("waitpid(child) returns 0", waitpid(pid) == 0);
  // A clearly non-existent PID should return -1.
  CHECK("waitpid(nonexistent) returns -1", waitpid(0x7fffffff) == -1);
}

/* Test 6: clone() failure on too-small stack. */
static void test_clone_small_stack_fails(void) {
  void *tiny = alloc_stack(16);
  int pid = clone(tiny, 16);
  CHECK("clone() with too-small stack returns -1", pid == -1);
}

int
main(void)
{
  printf(1, "== clone/waitpid/thread-group sharing tests ==\n");

  test_shared_memory();
  test_shared_fd_write_offset();
  test_shared_fd_close_effect();
  test_shared_cwd();
  test_waitpid_basic();
  test_clone_small_stack_fails();

  printf(1, "== done: %d failure(s) ==\n", fails);
  exit();
}
