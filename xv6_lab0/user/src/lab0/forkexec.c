#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  argv[0] = "echo";
  int pid = fork(); // fork returns pid

  if(pid < 0) {
    printf(2, "fork error\n");
    exit();
  }

  if(pid == 0) {
    exec("echo", argv);
    exit();
  }

  wait();
  exit();
}