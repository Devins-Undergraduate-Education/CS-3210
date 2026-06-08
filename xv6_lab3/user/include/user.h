struct stat;
struct rtcdate;
struct schedinfo;

// system calls
int fork(void);
int exit(void) __attribute__((noreturn));
int wait(void);
int waitinfo(struct schedinfo *);
int pipe(int *);
int write(int, const void *, int);
int read(int, void *, int);
int close(int);
int kill(int);
int exec(char *, char **);
int open(const char *, int);
int mknod(const char *, short, short);
int unlink(const char *);
int fstat(int, struct stat *);
int link(const char *, const char *);
int mkdir(const char *);
int chdir(const char *);
int dup(int);
int getpid(void);
char *sbrk(int);
int sleep(int);
int uptime(void);
int setscheduler(int, int, int);
int clone(void *, int);
int park(void *);
int setpark(void *);
int unpark(void *);
int waitpid(int);

// ulib.c
int stat(const char *, struct stat *);
char *strcpy(char *, const char *);
void *memmove(void *, const void *, int);
char *strchr(const char *, char);
int strcmp(const char *, const char *);
void printf(int, const char *, ...);
char *gets(char *, int);
unsigned int strlen(const char *);
void *memset(void *, int, unsigned int);
void *malloc(unsigned int);
void free(void *);
int atoi(const char *);
void itoa(int, char *);
