#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int
main(int argc, char *argv[])
{
  char buf[64]; // allocate room in memory
  char *msg = "Hello World!\n";
  int fd = open("temp_file", O_CREATE|O_RDWR); // returns a file_descriptor
  int write_file = write(fd, msg, strlen(msg));

  if(write_file != strlen(msg)) {
    printf(2, "write error\n");
    close(fd);
    exit();
  }
  close(fd);

  fd = open("temp_file", O_RDONLY);
  int read_file = read(fd, buf, sizeof(buf));

  if(read_file < 0) {
    printf(2, "read error\n");
    close(fd);
    exit();
  }
  close(fd);

  if(write(1, buf, read_file) != read_file) {
    printf(2, "write to stdout failed\n");
    exit();
  }

  exit();
}
