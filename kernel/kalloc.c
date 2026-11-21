// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
//
// Agent-OS 改造（模块 A：内存与地址空间）
//   1) per-CPU 页缓存：热路径只碰本核的锁，批量（4 页）从全局链补货，
//      缓存超过 64 页就回吐到 16 页。目的是把全局单锁的竞争从"每页一次"
//      降到"每 4 页一次"，同时避免在锁里做长循环。
//   2) clean / dirty 两条分配路径：
//        kalloc_dirty()  -- 返回的页内容未定义（调用方必须整页覆写）
//        kalloc_clean()  -- 返回的页已清零
//        kalloc()        -- 等价于 kalloc_clean()，保持 xv6 原语义
//      意义：uvmalloc 之类"随写随覆盖"的路径可以走 dirty，省掉一次 4 KiB memset；
//      页表页、trapframe 这类"先读后写"的必须走 clean。
//   3) kalloc_freepages()：供 Agent 观测，暴露当前空闲页数（不改变分配语义）。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// 全局空闲页链：只在 per-CPU 缓存未命中（冷路径）或回吐时才被访问。
struct {
  struct spinlock lock;
  struct run *freelist;
  uint64 npage;
} kmem;

#define KMEM_BATCH 4  // 一次从全局链取多少页
#define KMEM_HIGH 64  // 本核缓存超过这个数就回吐
#define KMEM_LOW 16   // 回吐到只剩这么多

// 每个 hart 一条私有链。kmemcpu[i] 只由 hart i 访问，锁仅用于打断抢占，
// 正常路径上几乎不会真正阻塞。
struct kmem_cpu {
  struct spinlock lock;
  struct run *freelist;
  int count;
};

struct kmem_cpu kmemcpu[NCPU];

// 把本核缓存回吐到全局链。调用时不能持有 kc->lock 或 kmem.lock。
static void
kmem_flush(int id)
{
  struct kmem_cpu *kc = &kmemcpu[id];
  struct run *head = 0, *tail = 0, *r;
  int n = 0;

  acquire(&kc->lock);
  while (kc->count > KMEM_LOW) {
    r = kc->freelist;
    kc->freelist = r->next;
    kc->count--;
    r->next = 0;
    if (tail)
      tail->next = r;
    else
      head = r;
    tail = r;
    n++;
  }
  release(&kc->lock);

  if (n == 0)
    return;

  acquire(&kmem.lock);
  tail->next = kmem.freelist;
  kmem.freelist = head;
  kmem.npage += n;
  release(&kmem.lock);
}

void
kinit(void)
{
  initlock(&kmem.lock, "kmem");
  kmem.freelist = 0;
  kmem.npage = 0;
  for (int i = 0; i < NCPU; i++) {
    initlock(&kmemcpu[i].lock, "kmemcpu");
    kmemcpu[i].freelist = 0;
    kmemcpu[i].count = 0;
  }

  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  struct kmem_cpu *kc;
  int flush;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;
  kc = &kmemcpu[cpuid()];

  acquire(&kc->lock);
  r->next = kc->freelist;
  kc->freelist = r;
  kc->count++;
  flush = kc->count > KMEM_HIGH;
  release(&kc->lock);

  if (flush)
    kmem_flush(cpuid());
}

// 全局链空了：把其他核缓存里的页全部搬回全局链（留给自己核的由调用方处理）。
// 只在真正的"没货"路径上调用，所以不心疼锁开销。
// 目的：让"分配失败"只发生在全系统真的没页的时候，而不是"本核缓存空了"。
static void
kmem_drain_others(int self)
{
  struct run *head = 0, *tail = 0;
  int n = 0;

  for (int id = 0; id < NCPU; id++) {
    struct kmem_cpu *kc = &kmemcpu[id];
    struct run *r, *chain = 0;

    if (id == self)
      continue;

    acquire(&kc->lock);
    while (kc->freelist) {
      r = kc->freelist;
      kc->freelist = r->next;
      kc->count--;
      r->next = chain;
      chain = r;
      n++;
    }
    release(&kc->lock);

    while (chain) {
      r = chain;
      chain = chain->next;
      r->next = 0;
      if (tail)
        tail->next = r;
      else
        head = r;
      tail = r;
    }
  }

  if (n == 0)
    return;

  acquire(&kmem.lock);
  tail->next = kmem.freelist;
  kmem.freelist = head;
  kmem.npage += n;
  release(&kmem.lock);
}

// 分配一页，内容未定义：调用方必须整页覆写（或在 clean 语义成立的地方使用）。
void *
kalloc_dirty(void)
{
  struct run *r;
  struct kmem_cpu *kc = &kmemcpu[cpuid()];

  for (int round = 0; round < 3; round++) {
    acquire(&kc->lock);
    r = kc->freelist;
    if (r) {
      kc->freelist = r->next;
      kc->count--;
      release(&kc->lock);
      return (void *)r;
    }
    release(&kc->lock);

    // 本核缓存空：先从全局链批量补货
    {
      struct run *batch[KMEM_BATCH];
      int n = 0;

      acquire(&kmem.lock);
      while (n < KMEM_BATCH && kmem.freelist) {
        batch[n++] = kmem.freelist;
        kmem.freelist = kmem.freelist->next;
        kmem.npage--;
      }
      release(&kmem.lock);

      if (n > 0) {
        acquire(&kc->lock);
        for (int i = 0; i < n; i++) {
          batch[i]->next = kc->freelist;
          kc->freelist = batch[i];
          kc->count++;
        }
        r = kc->freelist;
        kc->freelist = r->next;
        kc->count--;
        release(&kc->lock);
        return (void *)r;
      }
    }

    // 全局也空了：把别的核藏着的页收回来再试一次（退化为"全局一致的空闲视图"）
    kmem_drain_others(cpuid());
  }

  return 0;
}

// 分配一页并清零。
void *
kalloc_clean(void)
{
  void *p = kalloc_dirty();
  if (p)
    memset(p, 0, PGSIZE);
  return p;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// 保持 xv6 原语义：返回的页已清零。
void *
kalloc(void)
{
  return kalloc_clean();
}

// 当前空闲页总数（全局链 + 各核缓存）。观测用，不参与分配决策。
uint64
kalloc_freepages(void)
{
  uint64 n;

  acquire(&kmem.lock);
  n = kmem.npage;
  release(&kmem.lock);

  for (int i = 0; i < NCPU; i++) {
    acquire(&kmemcpu[i].lock);
    n += kmemcpu[i].count;
    release(&kmemcpu[i].lock);
  }

  return n;
}
