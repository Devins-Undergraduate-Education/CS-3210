# Lab0: Booting xv6 and Writing User-Space Programs

The goal of this lab is to get your environment setup, and gain some familiarity
with xv6, particularly the boot process (which you'll be modifying in lab
1), the tools we'll be using as a part of this course as well as how to write
user-space processes. 

## Requirements

In order to interface w/ our scripts, you must be able to run Bash and Docker.
Our recommendation for Windows users is to use WSL with Docker. Further help
can be provided through Piazza or Office Hours for getting this setup.

- Docker
- Bash
- Git

## Downloading, Compiling, and Running xv6

Start by checking out the xv6 repository on your local machine.

```bash
git clone https://github.gatech.edu/cs3210-fall2025/xv6.git
cd xv6
git checkout lab0
```

Next, launch the docker instance for the class using the provided script.

```bash
./scripts/docker.sh --pull # download the image from DockerHub
./scripts/docker.sh # run container and mount the pwd as /xv6
```

Now that you're inside the docker instance, build your repository. We recommend
building within a separate build directory:

```bash
cd /xv6
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

This will build and install the kernel.  

Once make complete successfully you can try to boot the kernel by
running our xv6-qemu script (from within your build directory):

```bash
./xv6-qemu
```

This should launch xv6 and take you to a prompt.  You can view available files
with `ls`.  You can close qemu by pressing CTRL-a followed by x.

## Building User-Space Programs in xv6
New user-space programs can be created by adding C source files in the `user` directory.
To ensure that the CMake build system is aware of the new file, and compiles it properly,
we need to modify the `user/Sources.cmake` file.

For the scope of this lab, the three mandatory questions are already added, and the optional file entries are commented.
We just need to specify the relative path of the user-level C source file that we want to include.

We've provided a template user-space program source file in the src directory, and these typically follow the traditional C program structure with the exception of slightly different header files.

After creating your program, we need to rebuild the kernel from the build directory with make. The build system will automatically compile your new program along with the existing user programs (NOTE: Ensure the program is in the `Sources.cmake` file). Once the build completes, you can run `xv6-qemu` and execute your program from the shell prompt to test for correctness.

For more details on the library functions and system calls available to a user-level program, refer to the `include/user.h` file.

# User-Space Programs

This section will refresh you C programming skills and make you familiar some of the
commonly used system calls in low-level C programming. Each program can be implemented in around 10 lines of code so they should not take very long. Most of your time may be spent learning about the different syscalls in xv6.

**IMPORTANT:** We will be explicitly requiring error handling in your code when handgrading. For any system calls/library functions that you utilize in your code, we are making it a requirement that you must gracefully handle any errors in the event these functions return any errors.

**Read chapter 0 of the xv6 manual. It provides an excellent overview of the xv6 operating system and describes the fundamental system calls that will be used throughout the lab.**

*Note: Be cautious when using Linux maunal page (man page), the xv6 syscalls are based of their Linux conterparts, but do not have all the features implemented, so use the man pages carefully.* 

## Part 1. Hello World (easy) [1 point]

The obvious starting point for a user-space program is a "Hello World" application, 
however we have put a slight twist to the common structure. 

* Write a user-space program `user/src/lab0/helloworld.c` that will write the "Hello World!" string into a temporary file using the write system call and then close the file. 
* Afterwards, the program should read the same string out of the file into a buffer and print it to the console.


## Part 2. Fork-Exec (easy) [1 point]

The fork system call is used to create new processes, known as child processes, 
from the currently existing process known as the parent. When fork is called, the operating system will create a new process, including a new address space, new file descriptors and other associated metadata for the process. These will be originally copied over from the parent process.

**We will explore fork() in greater detail for Lab2 and Lab3 so it is very important to be familiar with it.**

The exec system call is used to run an executable file in the context of the already existing process. When exec is called, the process ID (pid) does not change, however the code, heap and stack are replaced with the new program to be executed. 

Fork-Exec is a common primitive used in systems programming, as it provides a way of 
spawning a new process and executing a program. This is for example how the 
shell program (sh.c) works in xv6.

* Write a user-space program `user/src/lab0/forkexec.c` that will take as input a string from the command-line arguments and output it to the console.
* The program must use the fork-exec design pattern. Use the build-in echo command in order to display the text to the console. 

```
$ forkexec test input
test input
```

## Part 3. Ping-Pong (easy) [1 point]

Inter-process Communication (IPC), is a set of mechanisms provided by the operating system
to allow processes to share information with each-other. There are many methods of IPC including using signals, message queues and shared memory however we will be exploring the use of pipes for this example user-space program.

The pipe system call facilitates inter-process communication by creating a unidirectional
communication channel between two processes. It allows one process to write data 
into the pipe, while another process can read from it.

* Write a user-space program `user/src/lab0/pingpong.c` that will use pipes in order to commutate data across two processes, which must be created using fork.
* The processes should use pipes in order to share the \<pid\> with each-other. For simplicity, you may assume that the \<pid\> can fit in one byte, so only a single byte would need to be read from and written to the pipes.
* The parent should send its \<pid\> to the child through the pipe and the child should print "child: received ping from \<parent-pid\>".
* The child should then send its \<pid\> to the parent and the parent should print "parent: received pong from \<child-pid\>".
* Both processes should exit and terminate correctly. 

```
$ pingpong 
child: received ping from 8
parent: received pong from 9
```

## Part 4. Maximum Memory Size (moderate) [1 point]

The user-space programs execute in a virtual address space that is generated 
by the operating system. In xv6, calls to malloc eventually hit a call to the 
sbrk system call, which is intended to grow the processes virtual address space. However there is a limit to how much we can grow the address space, 
and that is limited by the maximum amount of physical memory in the system.

*Note: This is a rough approximation of the memory size, since part of the physical memory space is taken up by the kernel and memory-mapped I/O space. Also xv6 does not have a swap partition on the disk, so once physical memory runs out of pages, xv6 can not allocate any more memory. This is the limit you must hit.*


* Write a user-space program `user/src/lab0/limits.c` that will be used to approximate the maximum size of a process. 
* The program should keep allocating PGSIZE worth of data until it hits a limit, and this is denoted when the allocation function you use returns an error value (-1 for sbrk and 0 for malloc).
* The program should then print out the maximum attainable memory size in the following format: "Maximum Memory Size: 0x%x".

```
$ limits 
Maximum Memory Size: 0x00000000
```

## Extra Credit

Often there are limits to the abstractions that the operating system
provides. We explored an example of this in the previous section where
we calculated the maximum memory size of a user-space program. For
extra credit you should calculate the maximum number of files that can
be opened at once by a single process and the maximum number of processes at once.

### Maximum Number of Files Open at Once (moderate) [1 point]

Extend `user/src/lab0/limits.c` to calculate the maximum number of
files that can be opened at once by a single process in xv6.  Display
the result as "Maximum Number of Files Open at Once: %d".

*Note: This problem may be more challenging than it may seem at first
glance.  Keep in mind that, when run by the shell, your program may
start with some files open already.  You must comment your code to
explain how you have reached your final calculation.*

### Maximum Number of Processes (moderate) [1 point]

Extend the `user/src/lab0/limits.c` to calculate the maximum number of processes that 
can be created in xv6 at once. Display the result as "Maximum Number of Processes: %d".

*Note: This problem may be more challenging than it may seem at first glance. Keep in mind that there may exist other processes that are running in the background which you will need to take into account in your program. You must comment your code to explain how you have reached your final calculation.* 

```
$ limits 
Maximum Memory Size: 0x0000
Maximum Number of Files Open at Once: 0
Maximum Number of Processes: 0
```

## Grading

To submit, run `scripts/submit.sh` and upload the generated `submission.zip`
file to Gradescope.

## Debugging User-Space Programs

Debugging xv6 requires that we launch QEMU with the necessary flags such that it is able to sustain a remote gdb session.

```bash
./xv6-qemu -g
```

This should pause qemu from launching, and wait for a gdb session to attach. Now, we can connect to our Docker container in a separate terminal and launch gdb from the build directory.

```bash
./scripts/docker.sh --attach
cd /xv6/build
gdb
```

By default, gdb will be attached to debug the kernel, and gdb does not have any context 
and symbols related to the user-space programs, as they are compiled separately 
and loaded into xv6 through the file system. To get the symbols loaded into gdb 
for the user-space file we need to use the `file` command. 

Due to the way that gdb interacts with xv6, you will be able to set a breakpoint 
at a particular virtual address, in this case main() which is at 0x0, 
however other programs that run will also be aliased to the same virtual address,
since they each have their own address space. This may require you to run the continue
a few times before you hit the correct debug point.  

```bash
$ gdb
...
The target architecture is assumed to be i8086
[f000:fff0]    0xffff0: ljmp   $0xf000,$0xe05b
0x0000fff0 in ?? ()
+ symbol-file kernel

(gdb) file user/_helloworld
A program is being debugged already.
Are you sure you want to change the file? (y or n) y
Load new symbol table from "user/_helloworld"? (y or n) y
Reading symbols from user/_helloworld...

(gdb) br main
Breakpoint 1 at 0x0: file /xv6/user/src/lab0/helloworld.c, line 8.

(gdb) continue
Continuing.
```
