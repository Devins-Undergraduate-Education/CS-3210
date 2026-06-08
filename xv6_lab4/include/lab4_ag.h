#ifndef __INCLUDE_LAB4_AG_h_
#define __INCLUDE_LAB4_AG_h_

#include "types.h"
#include "stat.h"

void            report_disk_data_read();
void            report_disk_inode_read();
void            report_disk_data_write();
void            report_disk_inode_write();
int             report_stats(struct disk_stat*);

#endif  // __INCLUDE_LAB4_AG_h_
