// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld
extern char *zero_page;

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
  uint free_pages; 
  uint reference_count[PHYSTOP >> PGSHIFT];
} kmem;

static inline void check_pa(uint pa, char *who) {
  if(pa < (uint)V2P(end) || pa >= PHYSTOP) // validate phys_addr corresponds to page
    panic(who);
}

static inline uint refslot(uint pa) {
  return pa >> PGSHIFT; // convert phys_addr --> page-index
}

void dec_ref(uint phys_addr) {
  check_pa(phys_addr, "dec_ref");
  // CRIT
  acquire(&kmem.lock);
  uint idx = refslot(phys_addr);
  uint *slot = &kmem.reference_count[idx];
  *slot = *slot - 1; // dec count
  // END CRIT
  release(&kmem.lock);
}

void inc_ref(uint phys_addr) {
  check_pa(phys_addr, "inc_ref");
  // CRIT
  acquire(&kmem.lock);
  uint idx = refslot(phys_addr);
  uint *slot = &kmem.reference_count[idx];
  *slot = *slot + 1; // dec count
  // END CRIT
  release(&kmem.lock);
}

uint get_ref(uint phys_addr) {
  check_pa(phys_addr, "get_ref");
  // CRIT
  acquire(&kmem.lock);
  uint idx = refslot(phys_addr);
  uint count = kmem.reference_count[idx];
  // END CRIT
  release(&kmem.lock);
  return count;
}

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem"); // init lock
  kmem.use_lock = 0;
  kmem.free_pages = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1; // finally, use the lock
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE){
    kmem.reference_count[V2P(p) >> PGSHIFT] = 0; // inc ref
    kfree(p);
  }
}

// super duper cute helpers!!!
static inline uint pgindex_from_va(char *va) {
  return V2P(va) >> PGSHIFT;   // kmem.reference_count[]
}

static inline int is_valid_free_page(char *v) {
  return ((uint)v % PGSIZE) == 0 && v >= end && V2P(v) < PHYSTOP;
}

//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;

  if(!is_valid_free_page(v))
    panic("kfree");
  if(v == zero_page)
    return; // please do not free zero page (sadness)
  if(kmem.use_lock)
    acquire(&kmem.lock); // CRIT
    
  r = (struct run*)v;
  uint slot = pgindex_from_va(v);
  uint *refp = &kmem.reference_count[slot];

  // if someone holds the ref, DROP IT
  if(*refp > 0)
    *refp = *refp - 1;

  // refs = 0 = open buziness
  if(*refp == 0){
    // Fill with junk to catch dangling refs.
    memset(v, 1, PGSIZE);
    kmem.free_pages++;
    r->next = kmem.freelist;
    kmem.freelist = r;
  }

  if(kmem.use_lock)
    release(&kmem.lock); // END CRIT
}


// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock); // CRIT
  r = kmem.freelist;
  if(r) {
    kmem.freelist = r->next; // pop from freelist
    kmem.free_pages--;
    kmem.reference_count[pgindex_from_va((char*)r)] = 1; // inc ref
  }
  if(kmem.use_lock)
    release(&kmem.lock); // END CRIT
  return (char*)r;
}