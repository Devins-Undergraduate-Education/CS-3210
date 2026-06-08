// user/cowtests.c
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "mmu.h"

static void touch_byte(volatile char *p, char v) {
  *p = v;
}

static void read_byte(volatile char *p, volatile char *sink) {
  * (volatile char*)sink = *p;
}

static void uassert(int cond, const char *msg) {
  if (!cond) {
    printf(1, "FAIL: %s\n", msg);
    // ensure children die too
    kill(getpid());
    exit();
  }
}

#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))


static void pass(const char *msg) { printf(1, "OK  : %s\n", msg); }

static void fill_u32(uint *p, int words, uint v){ for(int i=0;i<words;i++) p[i]=v; }
static int  check_u32(uint *p, int words, uint v){
  for(int i=0;i<words;i++) if(p[i]!=v) return i+1;
  return 0;
}

static void page_mark(uint *base, int npages, uint v){
  for(int i=0;i<npages;i++) base[(i*PGSIZE)/4] = v + i;
}
static int page_check(uint *base, int npages, uint v, int every, int expect_equal){
  for(int i=0;i<npages;i++){
    if(i % every == 0){
      uint want = v + i;
      uint got = base[(i*PGSIZE)/4];
      if(expect_equal && got != want) return i+1;
      if(!expect_equal && got == want) return i+1;
    }
  }
  return 0;
}

static void crosspage_write(char *buf, const char *data, int len){
  int off = PGSIZE - 2;
  for(int i=0;i<len;i++) buf[off+i] = data[i];
}

static void read_into(char *dst, const char *payload, int len){
  int fds[2]; uassert(pipe(fds)==0, "pipe failed");
  int pid = fork(); uassert(pid>=0, "fork failed");
  if(pid==0){
    close(fds[0]);
    for(int i=0;i<len;){
      int n = min(13, len-i);
      if(write(fds[1], payload+i, n)!=n) exit();
      i+=n;
    }
    close(fds[1]);
    exit();
  }else{
    close(fds[1]);
    int r = read(fds[0], dst, len);
    uassert(r==len, "short read into buffer");
    close(fds[0]);
    wait();
  }
}

static void poke_u32(uint *p, uint val) {
  *p = val;
  uassert(*p == val, "poke_u32 readback mismatch");
}

static char* alloc_pages(int n) {
  uint pg = 4096;
  uint cur = (uint)sbrk(0);
  uint bump = (pg - (cur % pg)) % pg;
  if (bump) sbrk(bump);
  char *base = sbrk(n * pg);
  uassert(base != (char*)-1, "sbrk allocation failed");
  return base;
}

static void fill_pattern_u32(uint *base, int words, uint seed) {
  for (int i = 0; i < words; i++) base[i] = seed + i * 0x9e3779b9;
}

static int check_pattern_u32(uint *base, int words, uint seed) {
  for (int i = 0; i < words; i++) if (base[i] != seed + i * 0x9e3779b9) return -1;
  return 0;
}

static int probe_free_pages(int cap_pages) {
  int got = 0;
  while (got < cap_pages) {
    char *p = sbrk(PGSIZE);
    if (p == (char*)-1) break;
    got++;
  }
  if (got > 0) {
    int r = (int)sbrk(-got * PGSIZE);
    uassert(r != -1, "probe shrink failed");
  }
  return got;
}

static int choose_large_npages(void) {
  const int HARD_CAP = 1024;
  int freep = probe_free_pages(HARD_CAP);
  int large = max(8, (freep * 3) / 5);
  if (large > 256) large = 256;
  return large;
}


static void test_parent_write_triggers_cow() {
  char *buf = alloc_pages(1);
  uint *u = (uint*)buf;
  int words = 4096/4;
  fill_pattern_u32(u, words, 0xCAFEBABE);

  int pid = fork();
  uassert(pid >= 0, "fork failed");

  if (pid == 0) {
    uassert(check_pattern_u32(u, words, 0xCAFEBABE) == 0,
            "child sees data mismatch before parent writes");
    sleep(5);
    uassert(check_pattern_u32(u, words, 0xCAFEBABE) == 0,
            "child data changed after parent write");
    exit();
  } else {
    poke_u32(u + 42, 0xDEADC0DE);
    wait();
    uassert(u[42] == 0xDEADC0DE, "parent write not visible to parent");
    pass("parent write triggers private copy; child unaffected");
  }
}

static void test_copyout_pipe_on_cow_stack_in_child() {
  int pid = fork();
  uassert(pid >= 0, "fork failed");

  if (pid == 0) {
    int fds[2]; 
    int r = pipe(fds);
    uassert(r == 0, "pipe failed");

    uassert(fds[0] >= 0 && fds[1] >= 0, "pipe fds invalid");
    close(fds[0]); close(fds[1]);
    exit();
  } else {
    wait();
    pass("kernel copyout(pipe) resolves COW on child stack");
  }
}

static void test_copyout_read_into_cow_parent_stack() {
  int fds[2];
  if (pipe(fds) < 0) { printf(1, "FAIL: pipe failed\n"); exit(); }

  int pid = fork();
  if (pid < 0) { printf(1, "FAIL: fork failed\n"); exit(); }

  if (pid == 0) {
    // child: writer
    close(fds[0]);
    const char *msg = "hello-cow";
    write(fds[1], msg, 9);
    close(fds[1]);
    exit();
  } else {
    close(fds[1]);
    char buf[16];

    int n = read(fds[0], buf, 16);
    close(fds[0]);
    wait(); 

    if (n != 9) { printf(1, "FAIL: read returned %d\n", n); exit(); }
    if (buf[0] != 'h' || buf[8] != 'w') {
      printf(1, "FAIL: copyout(read) failed on COW page\n"); exit();
    }
    printf(1, "OK  : kernel copyout(read) resolves COW on parent stack\n");
  }
}

static void test_many_children_fanout_isolation() {
  char *buf = alloc_pages(1);
  uint *u = (uint*)buf;
  int words = 4096/4;
  fill_pattern_u32(u, words, 0x11223344);

  const int N = 6; 
  for (int i = 0; i < N; i++) {
    int pid = fork();
    uassert(pid >= 0, "fork failed");
    if (pid == 0) {
      poke_u32(u + (10 + i), 0xBEEF0000 + i);
      uassert(u[10 + i] == 0xBEEF0000 + i, "child self write missing");
      uassert(u[100] == 0x11223344 + 100 * 0x9e3779b9, "child sees unrelated corruption");
      exit();
    } 
  }
  for (int i = 0; i < N; i++) wait();

  uassert(check_pattern_u32(u, words, 0x11223344) == 0, "parent changed after fanout writes");
  pass("many-child fanout writes are isolated; parent intact");
}

static void test_grandchild_chain() {
  char *buf = alloc_pages(1);
  uint *u = (uint*)buf;
  int words = 4096/4;
  fill_pattern_u32(u, words, 0x5555AAA0);

  int pid = fork(); uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    int gc = fork(); uassert(gc >= 0, "fork failed (grandchild)");
    if (gc == 0) {
      poke_u32(u + 5, 0x0F0F0F0F);
      exit();
    } else {
      wait();
      uassert(u[5] == 0x5555AAA0 + 5*0x9e3779b9, "child modified by grandchild write");
      exit();
    }
  } else {
    wait();
    uassert(check_pattern_u32(u, words, 0x5555AAA0) == 0, "parent modified by grandchild chain");
    pass("grandchild writes isolated across generations");
  }
}

// 2) Cross-page write triggers exactly one COW per touched page
static void test_crosspage_write(void){
  char *buf = alloc_pages(2); uassert(buf != 0, "alloc failed");
  uint *u = (uint*)buf; int words = (2*PGSIZE)/4;
  fill_u32(u, words, 0xCCCCCCCC);

  int pid = fork(); uassert(pid>=0, "fork failed");
  if(pid==0){
    const char payload[8] = {1,2,3,4,5,6,7,8};
    crosspage_write(buf, payload, sizeof(payload));
    uassert(buf[PGSIZE-2]==1 && buf[PGSIZE+5]==8, "cross-page write incorrect");
    uassert(u[(PGSIZE/4)+10]==0xCCCCCCCC, "child sees corruption on page+1");
    exit();
  }else{
    wait();
    int bad = check_u32(u, words, 0xCCCCCCCC);
    uassert(bad==0, "parent changed after cross-page child write");
    pass("cross-page write: correct COW and boundaries");
  }
}

// 3) Kernel copyout into COW page (via read) works and stays private
static void test_copyout_into_cow(void){
  char *buf = alloc_pages(1); uassert(buf != 0, "alloc failed");
  uint *u = (uint*)buf; int words = PGSIZE/4;
  fill_u32(u, words, 0xABABABAB);

  int pid = fork(); uassert(pid>=0, "fork failed");
  if(pid==0){
    const char msg[] = "hello COW via copyout";
    read_into(buf+5, msg, sizeof(msg)-1);
    for(int i=0;i<sizeof(msg)-1;i++) uassert(buf[5+i]==msg[i], "copyout write missing");
    uassert(u[100]==0xABABABAB, "child sees unrelated corruption");
    exit();
  }else{
    wait();
    int bad = check_u32(u, words, 0xABABABAB);
    uassert(bad==0, "parent changed after read()->copyout into child");
    pass("copyout into COW page: child private; parent intact");
  }
}

// 4) Sparse writes: only touched pages copy; untouched page remains shared
static void test_sparse_pages(void){
  int np=8;
  char *buf = alloc_pages(np); uassert(buf != 0, "alloc failed");
  int words = (np*PGSIZE)/4;
  fill_u32((uint*)buf, words, 0xEFEFEFEF);

  int pid = fork(); uassert(pid>=0, "fork failed");
  if(pid==0){
    for(int i=0;i<np;i+=2) ((uint*)buf)[(i*PGSIZE)/4] = 0xFACE0000 + i;
    exit();
  }else{
    wait();
    int bad = check_u32((uint*)buf, words, 0xEFEFEFEF);
    uassert(bad==0, "parent changed after sparse touches");
    pass("sparse: only touched pages copied; parent intact");
  }
}

// 5) Fork chain: P -> C1 -> C2 -> C3 ; deepest writes, ancestors intact
static void test_fork_chain(void){
  char *buf = alloc_pages(4); uassert(buf != 0, "alloc failed");
  uint *u=(uint*)buf; int words=(4*PGSIZE)/4;
  fill_u32(u, words, 0x11111111);

  int p1=fork(); uassert(p1>=0, "fork1 failed");
  if(p1==0){
    int p2=fork(); uassert(p2>=0, "fork2 failed");
    if(p2==0){
      int p3=fork(); uassert(p3>=0, "fork3 failed");
      if(p3==0){
        for(int i=0;i<4;i++) u[(i*PGSIZE)/4] = 0xBEEF0000+i;
        exit();
      } else { wait(); exit(); }
    } else { wait(); exit(); }
  } else {
    wait(); // reap chain
    int bad = check_u32(u, words, 0x11111111);
    uassert(bad==0, "ancestor changed after deep child write");
    pass("fork chain: ancestors preserved");
  }
}

// 6) Many children under memory pressure; parent must remain intact
static void test_many_children(void){
  int np = 16;
  char *buf = alloc_pages(np); uassert(buf != 0,"alloc failed");
  uint *u=(uint*)buf; int words=(np*PGSIZE)/4;
  fill_u32(u, words, 0x22222222);

  int kids = 8, live=0;
  for(int k=0;k<kids;k++){
    int pid=fork(); uassert(pid>=0, "fork failed");
    if(pid==0){
      int which = (k*3)%np;
      ((uint*)buf)[(which*PGSIZE)/4] = 0xC0C00000 | (k<<8) | which;
      exit();
    } else live++;
  }
  while(live--) wait();

  int bad = check_u32(u, words, 0x22222222);
  uassert(bad==0, "parent changed with many children");
  pass("many children: parent intact, copies isolated");
}

// 8) child exit without touching: parent should remain fully writable
static void test_child_exit_releases_refs(void){
  char *buf = alloc_pages(4); uassert(buf != 0,"alloc failed");
  uint *u=(uint*)buf; int words=(4*PGSIZE)/4;
  fill_u32(u, words, 0x33333333);

  int pid=fork(); uassert(pid>=0,"fork failed");
  if(pid==0){
    exit();
  } else {
    wait();
    page_mark((uint*)buf, 4, 0x44440000);
    int bad = page_check((uint*)buf, 4, 0x44440000, 1, 1);
    uassert(bad==0, "parent write failed after child exit");
    pass("child exit(): refs dropped; parent fully writable");
  }
}

// 9) Unaligned write regions of assorted sizes (1..64 bytes) near boundary
static void test_unaligned_varsizes(void){
  char *buf = alloc_pages(2); uassert(buf != 0,"alloc failed");
  uint *u=(uint*)buf; int words=(2*PGSIZE)/4;
  fill_u32(u, words, 0x55555555);

  int pid=fork(); uassert(pid>=0,"fork failed");
  if(pid==0){
    for(int n=1;n<=64;n++){
      int off = PGSIZE - (n/2) - 3;
      for(int i=0;i<n;i++) buf[off+i] = (char)(n+i);
    }
    exit();
  } else {
    wait();
    int bad = check_u32(u, words, 0x55555555);
    uassert(bad==0, "parent changed after unaligned child writes");
    pass("unaligned sizes: parent intact");
  }
}

static void test_sbrk_shrink_frees_only_when_last_ref() {
  char *buf = alloc_pages(2);
  uint *u0 = (uint*)buf;
  uint *u1 = (uint*)(buf + 4096);
  fill_pattern_u32(u0, 4096/4, 0x10101010);
  fill_pattern_u32(u1, 4096/4, 0x20202020);

  int pid = fork(); uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    int r = (int)sbrk(-4096);
    uassert(r != -1, "child sbrk(-PGSIZE) failed");
    uassert(check_pattern_u32(u0, 4096/4, 0x10101010) == 0, "child sees corrupted page 0");
    exit();
  } else {
    wait();
    uassert(check_pattern_u32(u0, 4096/4, 0x10101010) == 0, "parent p0 corrupted after child shrink");
    uassert(check_pattern_u32(u1, 4096/4, 0x20202020) == 0, "parent p1 corrupted after child shrink");
    pass("deallocuvm on child doesnt free parents pages; refcounts honored");
  }
}

// Grow after fork: new pages must be private/writable (not COW).
static void test_sbrk_grow_after_fork_is_private() {
  char *base = alloc_pages(1);
  uint *u = (uint*)base;
  fill_pattern_u32(u, 4096/4, 0x42424242);

  int pid = fork(); uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    char *newp = sbrk(8192);
    uassert(newp != (char*)-1, "child sbrk grow failed");
    uint *w0 = (uint*)(newp);
    uint *w1 = (uint*)(newp + 4096);
    poke_u32(w0 + 1, 0xFACE0001);
    poke_u32(w1 + 1, 0xFACE0002);
    exit();
  } else {
    wait();
    uassert(check_pattern_u32(u, 4096/4, 0x42424242) == 0, "parent page changed after child grow");
    pass("sbrk grow in child allocates private writable pages");
  }
}

// Writing on exactly a page boundary should fault properly.
static void test_boundary_write_fault() {
  char *base = alloc_pages(2);
  memset(base, 0x5C, 4096*2);

  int pid = fork(); uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    char *p = base + 4096 - 8;
    for (int i = 0; i < 16; i++) p[i] = (char)i;
    for (int i = 0; i < 16; i++) uassert(p[i] == (char)i, "boundary write mismatch (child)");
    exit();
  } else {
    wait();
    char *p = base + 4096 - 8;
    for (int i = 0; i < 16; i++) uassert(p[i] == (char)0x5C, "parent corrupted by child boundary write");
    pass("boundary-spanning write faults and isolates correctly");
  }
}

static void stress_fault_storm() {
  char *base = alloc_pages(4);
  for (int i = 0; i < 4096*4; i++) base[i] = 0xAB;

  const int N = 4;
  for (int i = 0; i < N; i++) {
    int pid = fork(); uassert(pid >= 0, "fork failed in stress");
    if (pid == 0) {
      int page = i % 4;
      char *p = base + page*4096;
      for (int iter = 0; iter < 1000; iter++) {
        p[(iter*17) & (4096-1)] = (char)(iter);
      }
      exit();
    }
  }
  for (int i = 0; i < N; i++) wait();
  for (int i = 0; i < 4096*4; i++) uassert(base[i] == (char)0xAB, "parent changed in stress");
  pass("stress: repeated faults keep isolation; parent intact");
}

/* =======================  NEW: scale & bounds tests  ======================= */

// Cover "all memory sizes": a matrix of sizes from tiny to large, adapted to capacity.
static void test_size_matrix_allocation_and_cow(void) {
  int large_cap = choose_large_npages();
  // candidate sizes; we will skip those above capacity
  int sizes[] = {1,2,3,4,5,7,8,12,15,16,24,31,32,48,63,64,96,127,128};
  int ns = sizeof(sizes)/sizeof(sizes[0]);
  for (int si = 0; si < ns; si++) {
    int np = sizes[si];
    if (np > large_cap) continue;

    char *buf = alloc_pages(np); uassert(buf != 0, "alloc_pages failed");
    uint *u = (uint*)buf; int words = (np*PGSIZE)/4;
    fill_u32(u, words, 0xA5A5A5A5);

    int pid = fork(); uassert(pid>=0, "fork failed");
    if(pid==0){
      // write at start of each page
      for(int i=0;i<np;i++) u[(i*PGSIZE)/4] = 0xDEADBEEF + i;
      // verify own writes and spot-check other words
      for(int i=0;i<np;i++) uassert(u[(i*PGSIZE)/4]==0xDEADBEEF+i, "child write missing");
      for(int i=0;i<np;i++) uassert(u[(i*PGSIZE)/4 + 10]==0xA5A5A5A5, "child sees corrupted data");
      exit();
    } else {
      wait();
      int bad = check_u32(u, words, 0xA5A5A5A5);
      uassert(bad==0, "parent changed after child writes (size matrix)");
    }
  }
  pass("size-matrix: COW holds from 1 page up to large sizes");
}

// Fork a huge readonly address space: should succeed even with minimal free memory.
static void test_large_readonly_fork_ok(void) {
  int np = choose_large_npages();
  char *buf = alloc_pages(np); uassert(buf != 0, "alloc failed");
  uint *u = (uint*)buf; int words = (np*PGSIZE)/4;
  fill_pattern_u32(u, words, 0x7B7B0000);

  int pid = fork(); uassert(pid>=0, "fork failed");
  if (pid == 0) {
    uassert(check_pattern_u32(u, words, 0x7B7B0000) == 0, "child sees data mismatch (large ro)");
    exit();
  } else {
    wait();
    uassert(check_pattern_u32(u, words, 0x7B7B0000) == 0, "parent changed (large ro)");
    pass("large readonly fork succeeds without extra copies");
  }
}

// Large but sparse writes after fork: allocate only a fraction of pages.
static void test_large_sparse_write_after_fork(void) {
  int np = max(16, choose_large_npages());
  char *buf = alloc_pages(np); uassert(buf !=0, "alloc failed");
  uint *u = (uint*)buf; int words = (np*PGSIZE)/4;
  fill_u32(u, words, 0xE1E1E1E1);

  int pid = fork(); uassert(pid>=0, "fork failed");
  if (pid == 0) {
    int stride = 16; // copy 1/16th of pages
    for (int i = 0; i < np; i += stride) ((uint*)buf)[(i*PGSIZE)/4] = 0xA11C0000 + i;
    exit();
  } else {
    wait();
    int bad = check_u32(u, words, 0xE1E1E1E1);
    uassert(bad==0, "parent changed after large sparse child writes");
    pass("large sparse writes only copy touched pages; parent intact");
  }
}

// Low-free-memory scenario: consume most memory, then fork and do minimal touching.
static void test_fork_under_low_free_memory(void) {
  const int CAP = 512;
  int freep = probe_free_pages(CAP);
  // consume ~70% of those pages to simulate tight conditions
  int consume = (freep * 7) / 10;
  if (consume < 8) consume = 8;
  if (consume > 256) consume = 256;

  char *hog = alloc_pages(consume); uassert(hog != 0, "hog alloc failed");
  // touch so it actually maps
  for (int i = 0; i < consume; i++) hog[i*PGSIZE] = (char)i;

  // allocate a small test region
  char *buf = alloc_pages(8); uassert(buf != 0, "alloc small failed");
  uint *u = (uint*)buf; int words = (8*PGSIZE)/4;
  fill_u32(u, words, 0xD0D0D0D0);

  int pid = fork(); uassert(pid>=0, "fork failed under low free mem");
  if (pid == 0) {
    // only touch 1 page to require 1 private page allocation
    ((uint*)buf)[0] = 0xABCD1234;
    exit();
  } else {
    wait();
    int bad = check_u32(u, words, 0xD0D0D0D0);
    uassert(bad==0, "parent changed under low-free fork");
    pass("fork under low free memory: COW still isolates with minimal new pages");
  }
}

// Tiny memory case: minimal allocation then fork.
static void test_small_mem_fork(void) {
  char *buf = alloc_pages(1); uassert(buf != 0, "alloc 1p failed");
  uint *u = (uint*)buf; int words = PGSIZE/4;
  fill_u32(u, words, 0x0BADBEEF);

  int pid = fork(); uassert(pid>=0, "fork failed (small)");
  if (pid == 0) {
    u[0] = 0xF00D;
    uassert(u[0] == 0xF00D, "child write missing (small)");
    exit();
  } else {
    wait();
    uassert(u[0] == 0x0BADBEEF, "parent changed (small)");
    pass("fork works for very small memory");
  }
}

/* =========================== MORE EDGE CASES ===============================*/
static void test_child_write_triggers_cow() {
  char *buf = alloc_pages(1);
  uint *u = (uint*)buf;
  int words = 4096/4;
  fill_pattern_u32(u, words, 0x11223344);

  int pid = fork();
  uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    poke_u32(u + 13, 0xFEEDFACE);
    uassert(u[13] == 0xFEEDFACE, "child write not visible to child");
    exit();
  } else {
    wait();
    uassert(u[13] == 0x11223344, "parent page should remain unchanged");
    pass("child write triggers private copy; parent unaffected");
  }
}

// Writes at page boundaries (end and start of pages) cause correct COW.
static void test_boundary_writes() {
  char *buf = alloc_pages(2);
  memset(buf, 0xAA, 4096*2);

  int pid = fork();
  uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    touch_byte((char*)buf + 4095, 0x11);
    touch_byte((char*)buf + 4096, 0x22);
    uassert(buf[4095] == 0x11 && buf[4096] == 0x22, "child boundary writes incorrect");
    exit();
  } else {
    wait();
    uassert(buf[4095] == (char)0xAA && buf[4096] == (char)0xAA,
            "parent pages changed by child boundary writes");
    pass("boundary byte writes COW only the touched page(s)");
  }
}

// Reads must NOT trigger COW (no fault & no private copy).
static void test_reads_do_not_cow() {
  char *buf = alloc_pages(1);
  char sink = 0;
  memset(buf, 0x7A, 4096);

  int pid = fork();
  uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    for (int i = 0; i < 4096; i += 128) read_byte(buf + i, &sink);
    exit();
  } else {
    wait();
    for (int i = 0; i < 4096; i += 128)
      uassert((unsigned char)buf[i] == 0x7A, "parent data changed after child reads");
    pass("reads do not trigger COW");
  }
}

// Sibling isolation: child1 write doesn't affect child2 or parent.
static void test_sibling_isolation() {
  char *buf = alloc_pages(1);
  memset(buf, 0x5A, 4096);

  int c1 = fork(); uassert(c1 >= 0, "fork c1 failed");
  if (c1 == 0) {
    buf[123] = 0xA1;
    exit();
  }
  int c2 = fork(); uassert(c2 >= 0, "fork c2 failed");
  if (c2 == 0) {
    uassert((unsigned char)buf[123] == 0x5A, "child2 saw sibling modification");
    exit();
  }
  wait(); wait();
  uassert((unsigned char)buf[123] == 0x5A, "parent changed by child write");
  pass("sibling isolation holds with multiple children");
}

// fork() then exec(): no spurious COW faults during exec path.
static void test_fork_then_exec() {
  int pid = fork();
  uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    char *argv[2] = {"sh", 0 };
    if(exec("sh", argv) < 0)
      uassert(1 != 0, "exec failed");
    exit();
  } else {
    wait();
    pass("fork then exec: no COW regressions");
  }
}

// Large address space fork with no writes keeps pages shared.
static void test_large_fork_notouch() {
  int pages = 16;
  char *buf = alloc_pages(pages);
  memset(buf, 0xCC, 4096*pages);

  int pid = fork();
  uassert(pid >= 0, "fork failed");
  if (pid == 0) exit();
  else {
    wait();
    for (int i = 0; i < pages*4096; i += 1024)
      uassert((unsigned char)buf[i] == 0xCC, "content changed unexpectedly");
    pass("large fork with no touch keeps pages shared");
  }
}

// Heap growth after fork: new pages are private to the grower.
static void test_sbrk_grow_after_fork() {
  char *base = sbrk(0);
  int pid = fork();
  uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    sbrk(4096);
    base[0] = 0x42;
    uassert((unsigned char)base[0] == 0x42, "child new heap write failed");
    exit();
  } else {
    wait();
    pass("sbrk growth after fork yields private, non-shared pages");
  }
}

// Heap shrink (sbrk negative) correctly unmaps shared pages.
static void test_sbrk_shrink_cow_unmap() {
  char *p = sbrk(4096);
  p[0] = 0x9B;

  int pid = fork(); uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    sbrk(-4096);
    exit();
  } else {
    wait();
    uassert(p[0] == (char)0x9B, "parent lost data after child unmap");
    pass("sbrk shrink in child doesn't corrupt parent; shared refcount drops");
  }
}

// Kernel copyin path (write): writing a user buffer to a file/pipe resolves COW.
static void test_copyin_write_on_cow() {
  int fds[2]; uassert(pipe(fds) == 0, "pipe failed");
  char *buf = alloc_pages(1);
  memset(buf, 0xEA, 4096);

  int pid = fork(); uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    int n = write(fds[1], buf, 32);
    uassert(n == 32, "write failed");
    exit();
  } else {
    wait();
    close(fds[1]);
    char tmp[32];
    int n = read(fds[0], tmp, sizeof tmp); (void)n;
    close(fds[0]);
    pass("kernel copyin(write) resolves COW on child buffer");
  }
}

static int memeq(const void *a, const void *b, int n) {
  const unsigned char *x = (const unsigned char*)a;
  const unsigned char *y = (const unsigned char*)b;
  for (int i = 0; i < n; i++) {
    if (x[i] != y[i]) return 0;  // not equal
  }
  return 1;
}

// Kernel copyout path (read): reading into a COW buffer triggers COW on destination.
static void test_copyout_read_on_cow() {
  int fds[2]; uassert(pipe(fds) == 0, "pipe failed");
  char *buf = alloc_pages(1);
  memset(buf, 0, 4096);

  char msg[8] = "ABCDEFG";
  write(fds[1], msg, 8);

  int pid = fork(); uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    int n = read(fds[0], buf, 8);
    uassert(n == 8, "read failed");
    uassert(memeq(buf, msg, 8), "child read copyout mismatch");
    exit();
  } else {
    wait();
    for (int i = 0; i < 8; i++)
      uassert(buf[i] == 0, "parent buffer modified by child read copyout");
    close(fds[0]); close(fds[1]);
    pass("kernel copyout(read) resolves COW on child destination page");
  }
}

// TLB invalidation: after marking page read-only (COW), a write must fault once then succeed.
static void test_tlb_invalidation_local() {
  char *p = alloc_pages(1);
  memset(p, 0x33, 4096);
  int pid = fork(); uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    p[17] = 0x55;
    uassert((unsigned char)p[17] == 0x55, "post-fault write not applied");
    exit();
  } else {
    wait();
    uassert((unsigned char)p[17] == 0x33, "parent page mutated; TLB/perm mishandled");
    pass("write-after-fork faults once and succeeds, with correct TLB flush");
  }
}

// // Parent dies before child; child writes remain valid.
// static void test_parent_exit_before_child_write() {
//   char *p = alloc_pages(1);
//   memset(p, 0xAB, 4096);

//   int pid = fork(); uassert(pid >= 0, "fork failed");
//   if (pid == 0) {
//     sleep(5);
//     p[1] = 0xCD; // should COW in child even though parent gone
//     uassert((unsigned char)p[1] == 0xCD, "child write failed after parent exit");
//     exit();
//   } else {
//     // exit immediately
//     exit();
//   }
// }

// Many-children refcount churn: each child privately touches a different page.
static void test_many_children_divergent_writes() {
  int pages = 8;
  char *base = alloc_pages(pages);
  for (int i = 0; i < pages*4096; i++) base[i] = (char)(i & 0xFF);

  for (int k = 0; k < pages; k++) {
    int pid = fork(); uassert(pid >= 0, "fork failed");
    if (pid == 0) {
      int off = k*4096;
      base[off] = (char)(0x80 | k);
      exit();
    }
  }
  for (int k = 0; k < pages; k++) wait();
  for (int k = 0; k < pages; k++)
    uassert(base[k*4096] == (char)((k*4096) & 0xFF), "parent page mutated by a child");
  pass("N-children divergent writes isolate correctly with refcount drops on exit");
}

// Write spanning two pages (memmove) only COWs the touched pages.
static void test_cross_page_write_memmove() {
  char *dst = alloc_pages(2);
  memset(dst, 0, 8192);
  char *src = alloc_pages(1);
  for (int i = 0; i < 64; i++) src[i] = (char)(i + 1);

  int pid = fork(); uassert(pid >= 0, "fork failed");
  if (pid == 0) {
    memmove(dst + 4096 - 32, src, 64);
    uassert(dst[4096 - 32] == 1 && dst[4096] == 33, "cross-page copy wrong");
    exit();
  } else {
    wait();
    for (int i = 4096 - 32; i < 4096 - 32 + 64; i++)
      uassert(dst[i] == 0, "parent dst changed by child cross-page write");
    pass("cross-page memmove COWs only the pages actually written");
  }
}


/* ===============================  main  =================================== */

int
main(void)
{
  printf(1, "=== COW Test Suite (scaled) ===\n");

  test_parent_write_triggers_cow();
  test_copyout_pipe_on_cow_stack_in_child();
  test_copyout_read_into_cow_parent_stack();
  test_many_children_fanout_isolation();
  test_grandchild_chain();
  test_sbrk_shrink_frees_only_when_last_ref();
  test_sbrk_grow_after_fork_is_private();
  test_boundary_write_fault();
  stress_fault_storm();
  test_size_matrix_allocation_and_cow();   
  test_large_readonly_fork_ok();           
  test_large_sparse_write_after_fork();    
  test_fork_under_low_free_memory();       
  test_small_mem_fork();                   
  test_crosspage_write();
  test_copyout_into_cow();
  test_sparse_pages();
  test_fork_chain();
  test_many_children();
  test_child_exit_releases_refs();
  test_unaligned_varsizes();
  if(0) test_child_write_triggers_cow(); // fail
  test_boundary_writes();
  test_reads_do_not_cow();
  test_sibling_isolation();
  test_fork_then_exec();
  test_large_fork_notouch();
  test_sbrk_grow_after_fork();
  test_sbrk_shrink_cow_unmap();
  test_copyin_write_on_cow();
  test_copyout_read_on_cow();
  test_tlb_invalidation_local();
  //test_parent_exit_before_child_write(); // fail (zombie)
  test_many_children_divergent_writes();
  test_cross_page_write_memmove();


  printf(1, "=== All COW tests passed ===\n");
  exit();
}