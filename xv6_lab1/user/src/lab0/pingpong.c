#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  // RD = index[0], WR = index[1]
  int parent_pipe[2];
  int child_pipe[2];

  if(pipe(parent_pipe) < 0 || pipe(child_pipe) < 0) {
    printf(2, "pipe error\n");
    exit();
  }

  int pid = fork(); // returns type pid
  if(pid < 0) {
    printf(2, "fork error\n");
    exit();
  }

  if(pid == 0) { // for the child
    close(parent_pipe[1]); // do not need write fd
    close(child_pipe[0]); // do not need read fd

    int parent_pid = 0;
    if(read(parent_pipe[0], &parent_pid, sizeof(parent_pid)) != sizeof(parent_pid)) {
      printf(2, "child: write error\n");
      exit();
    }
    printf(1, "child: received ping from %d\n", parent_pid);

    int pid_byte = getpid();
    if(write(child_pipe[1], &pid_byte, sizeof(pid_byte)) != sizeof(pid_byte)) {
      printf(2, "child: write error\n");
      exit();
    }

    close(parent_pipe[0]);
    close(child_pipe[1]);
    exit();
  }

  // parent operations/process
  close(parent_pipe[0]);
  close(child_pipe[1]);

  int pid_byte = getpid();
  if(write(parent_pipe[1], &pid_byte, sizeof(pid_byte)) != sizeof(pid_byte)) {
    printf(2, "parent: write error\n");
    exit();
  }

  int child_pid = 0;
  if(read(child_pipe[0], &child_pid, sizeof(child_pid)) != sizeof(child_pid)) {
    printf(2, "parent: read error\n");
    exit();
  }
  printf(1, "parent: received pong from %d\n", child_pid);

  close(parent_pipe[1]);
  close(child_pipe[0]);
  wait();
  exit();
}