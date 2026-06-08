#include "types.h"
#include "stat.h"
#include "user.h"

#define PGSIZE 4096  // 4K Page size

int
main(int argc, char *argv[])
{
  int total = 0;

  while(1) {
    if((int)sbrk(PGSIZE) == -1) {
      break; // failure point
    }
    total += PGSIZE;
  }

  printf(1, "Maximum Memory Size: 0x%x\n", total);
  exit();
}