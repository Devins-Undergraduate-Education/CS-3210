#ifndef INCLUDE_STAT_h_
#define INCLUDE_STAT_h_

#define T_DIR  1   // Directory
#define T_FILE 2   // File
#define T_DEV  3   // Device

struct stat {
  short type;  // Type of file
  int dev;     // File system's disk device
  uint ino;    // Inode number
  short nlink; // Number of links to file
  ushort owner; // Owner uid
  ushort perms; // Permission bits for non-owners
  uint size;   // Size of file in bytes
};

struct disk_stat {
  int disk_data_read_count;
  int disk_data_write_count;
  int disk_inode_read_count;
  int disk_inode_write_count;  
  int free_blocks;
};

#endif  // INCLUDE_STAT_h_
