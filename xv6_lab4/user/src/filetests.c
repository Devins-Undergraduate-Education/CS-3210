#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"

#define STDOUT 1
#define USER1  1001
#define USER2  1002
#define USER3  1003

static int tests_passed;
static int tests_failed;

static void
expect(const char *msg, int ok)
{
  if(ok){
    tests_passed++;
    printf(STDOUT, "[OK]   %s\n", msg);
  } else {
    tests_failed++;
    printf(STDOUT, "[FAIL] %s\n", msg);
  }
}

// Helper to run a small callback after switching to uid.
static int
run_as_uid(int uid, int (*fn)(void *), void *arg)
{
  unsigned char code = 0;
  int p[2];

  if(pipe(p) < 0)
    return -1;

  int pid = fork();
  if(pid < 0)
    return -1;

  if(pid == 0){
    close(p[0]);
    if(uid >= 0 && setuid(uid) < 0){
      code = 0xff; // signal setup failure
      write(p[1], &code, 1);
      close(p[1]);
      exit();
    }
    int ret = fn(arg) & 0xff;
    code = (unsigned char)ret;
    write(p[1], &code, 1);
    close(p[1]);
    exit();
  }

  close(p[1]);
  int n = read(p[0], &code, 1);
  close(p[0]);
  wait();
  if(n != 1)
    return -1;
  return (int)code;
}

struct rw_args {
  const char *path;
  const char *data;
};

static int
read_attempt(void *arg)
{
  struct rw_args *rw = (struct rw_args *)arg;
  char buf[4];
  int fd = open(rw->path, O_RDONLY);
  if(fd < 0)
    return 0;
  int n = read(fd, buf, sizeof(buf));
  close(fd);
  if(n < 0)
    return 0;
  return 1;
}

static int
write_attempt(void *arg)
{
  struct rw_args *rw = (struct rw_args *)arg;
  // Open write-only so PROT_W is sufficient for non-owners.
  int fd = open(rw->path, O_WRONLY);
  if(fd < 0)
    return 0;
  int n = write(fd, rw->data, strlen(rw->data));
  close(fd);
  if(n < 0)
    return 0;
  return 1;
}

static int
read_as(int uid, const char *path)
{
  struct rw_args rw = {path, "x"};
  return run_as_uid(uid, read_attempt, &rw);
}

static int
write_as(int uid, const char *path, const char *data)
{
  struct rw_args rw = {path, data};
  return run_as_uid(uid, write_attempt, &rw);
}

static int
create_with_data(void *arg)
{
  struct rw_args *rw = (struct rw_args *)arg;
  int fd = open(rw->path, O_CREATE | O_RDWR);
  if(fd < 0)
    return 0;
  int n = write(fd, rw->data, strlen(rw->data));
  close(fd);
  if(n < 0)
    return 0;
  return 1;
}

static int
create_file_as(int uid, const char *path, const char *data)
{
  struct rw_args rw = {path, data};
  return run_as_uid(uid, create_with_data, &rw);
}

struct chmod_args {
  const char *path;
  int mode;
};

static int
chmod_attempt(void *arg)
{
  struct chmod_args *ca = (struct chmod_args *)arg;
  int r = chmod(ca->path, ca->mode);
  return r == 0;
}

static int
chmod_as(int uid, const char *path, int mode)
{
  struct chmod_args ca = {path, mode};
  return run_as_uid(uid, chmod_attempt, &ca);
}

struct chown_args {
  const char *path;
  int new_owner;
};

static int
chown_attempt(void *arg)
{
  struct chown_args *co = (struct chown_args *)arg;
  int r = chown(co->path, co->new_owner);
  return r == 0;
}

static int
chown_as(int uid, const char *path, int owner)
{
  struct chown_args co = {path, owner};
  return run_as_uid(uid, chown_attempt, &co);
}

static int
mkdir_attempt(void *arg)
{
  const char *path = (const char *)arg;
  return mkdir(path) == 0;
}

static int
mkdir_as(int uid, const char *path)
{
  return run_as_uid(uid, mkdir_attempt, (void *)path);
}

static int
copy_file(const char *src, const char *dst)
{
  char buf[512];
  int r = -1;
  int in = open(src, O_RDONLY);
  if(in < 0)
    return -1;

  int out = open(dst, O_CREATE | O_RDWR);
  if(out < 0){
    close(in);
    return -1;
  }

  while((r = read(in, buf, sizeof(buf))) > 0){
    if(write(out, buf, r) != r){
      r = -1;
      break;
    }
  }

  close(in);
  close(out);
  if(r < 0)
    return -1;
  return 0;
}

struct exec_args {
  const char *path;
  const char *arg;
};

static int
exec_attempt(void *arg)
{
  struct exec_args *ea = (struct exec_args *)arg;
  int p[2];
  if(pipe(p) < 0)
    return 0;

  int pid = fork();
  if(pid < 0)
    return 0;

  if(pid == 0){
    close(p[0]);
    close(1);
    dup(p[1]);   // stdout -> pipe
    close(p[1]);
    char *argv[] = {(char *)ea->path, (char *)ea->arg, 0};
    if(exec((char *)ea->path, argv) < 0){
      write(1, "E", 1);
    }
    exit();
  }

  close(p[1]);
  char outbuf[8];
  int n = read(p[0], outbuf, sizeof(outbuf));
  close(p[0]);
  wait();
  if(n <= 0)
    return 0;
  // Exec succeeded if we saw something other than our error byte.
  return outbuf[0] != 'E';
}

static int
exec_as(int uid, const char *path)
{
  struct exec_args ea = {path, "OK"};
  return run_as_uid(uid, exec_attempt, &ea);
}

static void
test_default_new_file_permissions(void)
{
  const char *path = "ft_default_perm";
  unlink(path);

  expect("root created new file", create_file_as(0, path, "rootdata") == 1);
  expect("non-owner blocked from reading new file",
         read_as(USER1, path) == 0);
  expect("non-owner blocked from writing new file",
         write_as(USER1, path, "x") == 0);

  unlink(path);
}

static void
test_owner_access_without_perms(void)
{
  const char *path = "ft_owner_access";
  unlink(path);

  expect("non-root user created file",
         create_file_as(USER1, path, "hello") == 1);
  expect("owner can read even with no PROT bits",
         read_as(USER1, path) == 1);
  expect("owner can write even with no PROT bits",
         write_as(USER1, path, "Z") == 1);
  expect("different user cannot read without permission",
         read_as(USER2, path) == 0);

  unlink(path);
}

static void
test_chmod_and_permissions(void)
{
  const char *path = "ft_chmod";
  unlink(path);

  expect("set up file for chmod test",
         create_file_as(USER1, path, "data") == 1);

  expect("root set PROT_R", chmod(path, PROT_R) == 0);
  expect("non-owner read succeeds with PROT_R",
         read_as(USER2, path) == 1);
  expect("non-owner write blocked with only PROT_R",
         write_as(USER2, path, "a") == 0);

  expect("root set PROT_W", chmod(path, PROT_W) == 0);
  expect("non-owner read blocked with only PROT_W",
         read_as(USER2, path) == 0);
  expect("non-owner write succeeds with PROT_W",
         write_as(USER2, path, "b") == 1);

  expect("root set PROT_R|PROT_W", chmod(path, PROT_R | PROT_W) == 0);
  expect("non-owner read succeeds with PROT_R|PROT_W",
         read_as(USER2, path) == 1);
  expect("non-owner write succeeds with PROT_R|PROT_W",
         write_as(USER2, path, "c") == 1);

  expect("non-owner chmod fails",
         chmod_as(USER2, path, 0) == 0);

  unlink(path);
}

static void
test_chown_rules(void)
{
  const char *path = "ft_chown";
  unlink(path);

  expect("user1 created file for chown test",
         create_file_as(USER1, path, "owner") == 1);

  expect("non-owner chown rejected",
         chown_as(USER2, path, USER2) == 0);
  expect("non-owner chmod rejected",
         chmod_as(USER2, path, PROT_R) == 0);
  expect("root chowns to user3",
         chown(path, USER3) == 0);
  expect("owner changed, user3 can write",
         write_as(USER3, path, "Y") == 1);
  expect("invalid uid rejected",
         chown(path, 0x1FFFF) < 0);

  unlink(path);
}

static void
test_directory_access_rules(void)
{
  const char *dir = "ft_dirperm";
  const char *file_path = "ft_dirperm/file";
  unlink(file_path);
  unlink(dir);

  expect("user1 created directory", mkdir_as(USER1, dir) == 1);

  expect("user2 cannot create file when dir perms are empty",
         create_file_as(USER2, file_path, "x") == 0);

  expect("root sets dir PROT_R", chmod(dir, PROT_R) == 0);
  expect("user2 still cannot create without PROT_W",
         create_file_as(USER2, file_path, "y") == 0);

  expect("root sets dir PROT_R|PROT_W", chmod(dir, PROT_R | PROT_W) == 0);
  expect("user2 can create file once dir is readable+writeable",
         create_file_as(USER2, file_path, "z") == 1);

  unlink(file_path);
  unlink(dir);
}

static void
test_exec_permissions(void)
{
  const char *path = "ft_execbin";
  unlink(path);

  expect("copied echo for exec test", copy_file("echo", path) == 0);
  expect("chown exec test file to user1", chown(path, USER1) == 0);
  expect("clear permissions", chmod(path, 0) == 0);

  expect("non-owner exec denied without PROT_R",
         exec_as(USER2, path) == 0);

  expect("enable read permission", chmod(path, PROT_R) == 0);
  expect("non-owner exec succeeds with PROT_R",
         exec_as(USER2, path) == 1);

  unlink(path);
}

int
main(void)
{
  printf(STDOUT, "Running file permission tests (current uid=%d)\n", getuid());

  test_default_new_file_permissions();
  test_owner_access_without_perms();
  test_chmod_and_permissions();
  test_chown_rules();
  test_directory_access_rules();
  test_exec_permissions();

  printf(STDOUT, "File permission tests complete: %d passed, %d failed\n",
         tests_passed, tests_failed);
  exit();
}
