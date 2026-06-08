<!-- Added -->
# Lab 4 -- User-Isolation, File Permissions and Large File Support

The purpose of this lab is to have you explore how the kernel can enable
user-space applications to build their own isolation and login systems through
simple, minimal, low-level kernel interfaces. To accomplish this you will
create a simple (but realistic) user system, a file permission system, a
a user-space login system, and add support for large files.

We have provided primitive cryptographic tools for hashing and encrypting data.
The expectation is for you to utilize these tools when building your login
system to safely but efficiently store user data.

NOTE: For simplicity, and to avoid autograder build failures, the branch for
Lab 4 has the userspace components for all new system calls already
implemented. You are expected to add the kernel-side component of these calls.

## Part 1 -- Adding Users

In the first portion of this lab you will be providing each process with a
logical user identifer (henceforth `uid`). There will be two functions which
allow a user-space process within xv6 to modify or view the current uid `uid`:

```c
/**
 * Sets the uid of the current process
 * 
 * @param uid The uid to change to. Valid ranges are 0x0 - 0xFFFF
 * @returns 0 on success, -1 on failure
 */
int setuid(int);

/**
 * Gets the uid of the currently running process.
 *
 * @returns The current process's uid.
 */
int getuid(void);
```

Beyond this description the following rules should be applied for uids:

- The uid of `init` is 0 and is considered the root process.
- Each child process inherits its parent's `uid`
- Only processes with `uid` 0 may change their `uid` through `setuid`

## Part 2 -- File Permissions

Now that you have a basic user identifier system working, you'll be adding file
permissions to the system which leverage the `uid` feature to logically isolate
certain files.

You will need to add several properties to each file and directory within xv6.
First, the `owner` field will determine which uid `owns` a specific file.
Second, the `permission` field will determine the access privileges non-owners
have for a specific file.

### Logical Permissions

The permission system logically works in the following way:

- If a file is accessed by its owner or `uid` 0 (henceforth `root`), then that
  access is permitted
- If a file is accessed for reading by another user (not its owner or root)
  and its read permission is set, that access is permitted
- If a file is accessed for writing by another user (not its owner or root)
  and its write permission is set, that access is permitted
- All other accesses are not permitted

You will need to identify any high-level operations which involve file accesses.
If an operation accesses a file, you must confirm that the specific access type
performed is permitted for the given process. A sample of operations involving
file accesses are included below:

- directory reads (read)
- open (read and/or write, depending on the flags)
- file creation (write to file's directory)
- exec (read)

If an operation is not permitted, the system call should return -1, and no
changes to the disk or file-system state should occur. If an operation is
permitted, the operation should complete as it did before the permission system
was added.

**NOTE**:

- Directory reads include the "path walk" a filesystem does to open a file in
a nested directory
- When creating or removing a file, the file's full directory path must be
readable but ONLY the file's immediate directory needs to be both readable and
writeable

By default, when the build system creates the disk image `user/fs.img` that xv6
will use in the build directory, all files should be owned by root and both
readable and writeable for any user. After the disk image is built and xv6
starts, all newly created files should be owned by the process that created the
file and have `PROT_R` and `PROT_W` both cleared. Details on the disk image and
the `PROT_R` and `PROT_W` flags can be found below.

### System Call Interface

To control the permissions of a file, you'll be adding two new system calls
to the xv6 filesystem:

```c
/**
 * Changes the owner of the file at filename to uid. On failure, no permissions
 * are changed.
 *
 * @param filename A filesystem path naming the file to change
 * @param uid The new owner of the file, valid range is 0x0-0xFFFF
 * @returns 0 on success, -1 on failure
 */

int chown(const char *filename, int uid);

/**
 * Changes the permissions of the file at filename to perm. On failure, no
 * permissions are changed.
 *
 * @param filename A filesystem path naming the file to change
 * @param perm The new permissions for the file.
 * @returns 0 on success, -1 on failure.
 */

int chmod(const char *filename, int perm);
```

Here a filesystem path refers to either a relative path (e.g. `"testdir/file1"`)
or an absolute path (e.g. `"/home/ddevec/testdir/file1"`).

Both of these system calls change the file's `owner` and `permission` properties
respectively. The permission flags used by `chmod` are `PROT_W` and `PROT_R` as
defined in `include/fs.h`.

Additionally, the following rules should apply to `chmod` and `chown`:

- If an invalid uid is passed to `chown`, the system call will fail
- Only a file's owner or root may successfully change a file's permission or
  owner. All attempts by other users should fail with a return code of -1
- Any changes to a files permission or owner are expected to persist on restart

### Physical Disk Layout Requirements

All changes to file ownership or permissions should be atomically persisted to
disk (using xv6's built-in log system will be sufficient for this).

To facilitate grading, we require a specific physical disk layout for our files.
In particular, the on-disk layout of the file's inode must be:

```txt
+0x00 - 2bytes - type  -- file type
+0x02 - 2bytes - major -- major device number
+0x04 - 2bytes - minor -- minor device number
+0x06 - 2bytes - nlink -- number of links to inode in the file system
+0x08 - 4bytes - size  -- size of the file in bytes
+0x0c - 2bytes - owner -- Owner uid of the file
+0x0e - 2bytes - perms -- Permissions of the file
+0x10 - 48bytes - addres -- Array of data block addresses + indirect block address
```

Additionally, the following requirements must be followed for all file system
operations:

- Operations must persist across restarts
- All filesystem operations (including `chown` and `chmod`) must be persistent
  and atomic. We recommend exploring xv6's `log` infrastructure (and associated
  video) to achieve atomic persistent filesystem operations in xv6.

**NOTE**:

You will have to modify mkfs (`tools/mkfs.c`) to support your new
filesystem inode layout. You should modify it to default all files to being
owned by root and having both the read and write permission bits set.

Your filesystem will persist across reboots to xv6. If you need to recreate your
initial filesystem, you can perform a clean build or remove the disk image
`user/fs.img` located within your build directory by doing
`rm build/user/fs.img`.

## Part 3 -- Login

In xv6, the initial user-space process launches the shell directly without
considering what user is currently logged in. In this part, you are expected to
add login functionality by leveraging parts 1 & 2 in addition to provided
user-space cryptographic tools for hashing (`sha256`) and encrypting
(`aes256_{encrypt,decrypt}`) available in `user/include/crypto.h`.

Your key contributions will be decision making in the design and implementation
of a secure mechanism for storing and retrieving passwords in xv6. Please refer
to content discussed in the security lectures as well as resources online
regarding Linux's own password system for inspiration. The autograder will be 
testing for correctness of your implementation being able to facilitate login, 
but emphasis will be put on handgrading to check for secureness of your design. 
Please include `login_design.md` in your final submission which describes what 
mechanisms you utilized to secure your system, which files are created in the 
process, and what the content of those files mean.

Instead of launching xv6 by using `xv6-qemu`, we have provided an alternative
script `login-xv6-qemu` which relies on `user/src/login/login_init.c` and
`user/src/login/login.c` to facilitate a login system which would look like
the following:

```txt
init -> login -> (downgrade permission from root to user's uid) -> shell
```

This existing login infrastructure will hook into user-space functions which
you will be expected to implement. The lifecycle of the login system interleaved
with these functions (which are denoted with square brackets e.g. \[init_hook\])
looks like the following:

```txt
init:
  [init_hook()]
  <infinite loop> {
    login:
      <fetch username from console>
      if [does_user_exist(username)] {
        <prompt for password>
      } else {
        <prompt for new password>
        [create_user(username, password)]
      }
      [login_user(username, password)]
  }
```

```c
/**
 * Hook into user/src/login/login_init.c in order to intialize any files or
 * data structures necessary for the login system
 * 
 * Called once per boot of xv6
 */
void init_hook();

/**
 * Check if user exists in system 
 * 
 * @param username A null-terminated string representing the username
 * @return 0 on success if user exists, -1 for failure otherwise
 */
int does_user_exist(char *username);

/**
 * Create a user in the system associated with the username and password. Cannot
 * overwrite an existing username with a new password. Expectation is for
 * created users to have a unique non-root uid.
 * 
 * @param username A null-terminated string representing the username
 * @param password A null-terminated string representing the password
 * @return 0 on success, -1 for failure
 */
int create_user(char *username, char *password);

/**
 * Login a user in the system associated with the username and password. Launch
 * the shell under the right permissions for the user. If no such user exists
 * or the password is incorrect, then login will fail.
 * 
 * @param username A null-terminated string representing the username
 * @param password A null-terminated string representing the password
 * @return no return on success, -1 for failure
 */
int login_user(char *username, char *password);
```

**Note**:

- No mechanisms are in place for logging out. The expectation is to reboot xv6.
- By default, there should exist a root user with username of `root` and
password of `admin`. This user will maintain the uid of 0 and will not have to
be created by manual entry.
- Created users should persist across reboots
- Prior to the shell being launched, the user should have its permissions
lowered to its uid
- Changes made in the `xv6-qemu` file system will not persist to
`login-xv6-qemu` and vice versa since they use different file system images.

<!-- Added -->
## Part 4 -- Large File Support

Currently, the file system in xv6 is designed such that the maximum file size is only 140 sectors/blocks (in xv6, the size of a block and disk sector coincide). What if we want to store a large file, e.g., an image? In this part of the lab, you will be adding support for much larger file sizes. 

### Free Blocks on Disk

<!-- TODO: Replace with correct name and syscall -->
To facilitate the tests for this part of the lab, you will first develop a simple reporting tool to check the number of free sectors on disk. For this, you must implement the function ```get_free_blocks``` as described below:

```c
/**
 * Report the number of free blocks on the disk
 * 
 * @return number of free blocks (integer)
 */
int get_free_blocks();
```

You will only need to implement this function, which will then be used in the syscall ```report_stats```, used for testing correctness. **Do not modify** the ```report_stats``` function. 

### File System Changes

We would now like to support file sizes upto 8MB (1MB = $2^{20}$ bytes). To do so, you must identify the following:

1. How is the limit of the file system size specified in xv6? 
2. What determines the maximum size of any single file on xv6 (or for any Unix-like OS for that matter)? **Contraint**: The *size* of this structure **must not change**. 
3. What functionality must be changed to ensure that more disk space can be taken up by a file than currently possible? (Hint: identify how space on disk is allocated/reclaimed)

### Design Choices

Inherent to this part of the lab is a fundamental design choice which will accordingly determine the maximum file size that can be supported. We have provided the syscall ```report_stats``` to report the number of disk inode reads, disk inode writes, disk data reads and disk data writes at any point in the system, for which the relevant functionality is defined in ```lab4_ag.c``` and other FS-related kernel source files. The syscall makes use of the struct ```disk_stat``` defined in ```stat.h```. This information will be used to autograde the design choice made; the design choice should not result in a significantly high number of disk I/Os. 

<!-- For this part of the lab, you will have to compare (at least) two different designs and write up a basic report describing the pros and cons of the different designs with respect to the metrics observed for these two workloads, explaining briefly why these differences arise. We also encourage you to develop more workloads to compare the different designs.  -->

## Autograder

As usual, you will submit this lab to the autograder. The tests in the
autograder are categorized in the following manner:

- User: 1 - 4
- Filesystem: 5 - 17
- Login: 18 - 27
- Large File Support: 28 - 32

When submitting utilize the provided `scripts/submit.sh`. If you do not have
Python 3.7+ on your local machine, run this script in the Docker container.

**Note**:

Even though these are categorizations of the tests, you can assume subsequent
parts can rely on elements from previous parts as was the case in previous labs.

## Hand Grading

Lab 4 also includes a hand-grading portion. Hand-grading for Lab 4 will check
for code/implementation correctness and will involve a review of `login_design.md`.
