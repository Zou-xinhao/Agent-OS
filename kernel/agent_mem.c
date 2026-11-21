//
// Agent-OS：内存与地址空间（赛题任务一 + 任务三的配额部分）
// 负责人：A
//
// 本文件只放"机制"，不放"策略"：
//   机制（本文件）
//     - PCB 上 5 个 Agent 字段与 ctx_meta 记账
//     - Context 区的地址空间预留与页映射（16 页 = 64 KiB，用户态只读、内核可写）
//     - 上下文节点的线性 FIFO：追加 / 按序号取 / 回滚 / 清空
//     - 配额判定与淘汰（唯一判据 ctx_meta.used）
//   策略（B / 用户态负责）
//     - 节点内容的语义、128 B 摘要、关键词倒排、检索与缓存
//
// 地址空间约定
//   Context 区固定在 TRAPFRAME 下方：[AGENT_CTX_BASE, AGENT_CTX_BASE + 64 KiB)
//   - 与堆（sbrk 自 sz 向上）和栈（sz 附近）都不重叠，sbrk 检查逻辑一个字都不用改；
//   - 页表项是 PTE_R | PTE_U，没有 PTE_W：用户态只能读，内核通过物理直映（pa）
//     写入，于是"用户态能不能改自己的上下文"这件事由硬件页表保证，而不是靠约定。
//   - 进程退出/exec 时由 agent_ctx_release() 显式归还这 16 页。
//
// 节点格式（Context 区内，从 head 到 tail 线性排列）
//   [u32 len][u32 flags][len 字节数据，向上 8 字节对齐]
//   ctx_meta.used == tail - head，quota 判定只看这一个量。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "agent_proto.h"
extern struct proc proc[NPROC]; // 进程表（dev 的 defs.h 未导出）

#define CTX_HDR 8u // 节点头：[len][flags]

static uint32
ctx_pad(uint32 n)
{
  return (n + 7u) & ~7u;
}

// 把 Context 区内的偏移翻译成内核可直接读写的指针。
// 内核页表把 PHYSTOP 之前的物理内存恒等映射，所以 pa 本身就是可用的内核地址。
static char *
ctx_kaddr(struct proc *p, uint32 off)
{
  uint64 pa;

  if (p->ctx.size == 0 || off >= p->ctx.size)
    return 0;
  pa = walkaddr(p->pagetable, p->ctx.base + off);
  if (pa == 0)
    return 0;
  return (char *)(pa + (off % PGSIZE));
}

// 从内核缓冲写入 Context 区（可跨页）。
static int
ctx_put(struct proc *p, uint32 off, char *buf, uint32 n)
{
  while (n > 0) {
    char *k = ctx_kaddr(p, off);
    uint32 chunk;

    if (k == 0)
      return -1;
    chunk = PGSIZE - (off % PGSIZE);
    if (chunk > n)
      chunk = n;
    memmove(k, buf, chunk);
    off += chunk;
    buf += chunk;
    n -= chunk;
  }
  return 0;
}

// 从 Context 区读到内核缓冲（可跨页）。
static int
ctx_get(struct proc *p, uint32 off, char *buf, uint32 n)
{
  while (n > 0) {
    char *k = ctx_kaddr(p, off);
    uint32 chunk;

    if (k == 0)
      return -1;
    chunk = PGSIZE - (off % PGSIZE);
    if (chunk > n)
      chunk = n;
    memmove(buf, k, chunk);
    off += chunk;
    buf += chunk;
    n -= chunk;
  }
  return 0;
}

// 用户缓冲 -> Context 区；用户缓冲先校验可读（copyin 内部逐页检查）。
static int
ctx_put_user(struct proc *p, uint32 off, uint64 uaddr, uint32 n)
{
  while (n > 0) {
    char *k = ctx_kaddr(p, off);
    uint32 chunk;

    if (k == 0)
      return -1;
    chunk = PGSIZE - (off % PGSIZE);
    if (chunk > n)
      chunk = n;
    if (copyin(p->pagetable, p->sz, k, uaddr, chunk) < 0)
      return -1;
    off += chunk;
    uaddr += chunk;
    n -= chunk;
  }
  return 0;
}

// Context 区 -> 用户缓冲。
static int
ctx_get_user(struct proc *p, uint32 off, uint64 uaddr, uint32 n)
{
  while (n > 0) {
    char *k = ctx_kaddr(p, off);
    uint32 chunk;

    if (k == 0)
      return -1;
    chunk = PGSIZE - (off % PGSIZE);
    if (chunk > n)
      chunk = n;
    if (copyout(p->pagetable, p->sz, uaddr, k, chunk) < 0)
      return -1;
    off += chunk;
    uaddr += chunk;
    n -= chunk;
  }
  return 0;
}

// 把 [head, tail) 平移到区首，抵消历史淘汰留下的空洞。
// 目的：让 tail + need <= size 重新成立。均摊成本 O(1)（只在快写满时触发一次）。
static int
ctx_compact(struct proc *p)
{
  uint32 len = p->ctx.tail - p->ctx.head;
  uint32 i = 0;

  if (len == 0) {
    p->ctx.head = 0;
    p->ctx.tail = 0;
    return 0;
  }

  while (i < len) {
    char *src = ctx_kaddr(p, p->ctx.head + i);
    char *dst = ctx_kaddr(p, i);
    uint32 chunk;

    if (src == 0 || dst == 0)
      return -1;
    chunk = PGSIZE - ((p->ctx.head + i) % PGSIZE);
    if (chunk > PGSIZE - (i % PGSIZE))
      chunk = PGSIZE - (i % PGSIZE);
    if (chunk > len - i)
      chunk = len - i;
    // dst < src，逐段向低地址搬，memmove 允许重叠
    memmove(dst, src, chunk);
    i += chunk;
  }

  p->ctx.tail = len;
  p->ctx.head = 0;
  p->ctx.used = len;
  return 0;
}

// 丢掉最老的一个节点（配额淘汰 / 自动淘汰）。
static void
ctx_evict_head(struct proc *p)
{
  uint32 hdr[2];

  if (p->ctx.count == 0)
    return;
  if (ctx_get(p, p->ctx.head, (char *)hdr, CTX_HDR) != 0) {
    // 记账与页表不一致时直接清空，宁可丢上下文也不要越界
    p->ctx.head = p->ctx.tail = p->ctx.used = p->ctx.count = 0;
    return;
  }
  p->ctx.head += CTX_HDR + ctx_pad(hdr[0]);
  p->ctx.used = p->ctx.tail - p->ctx.head;
  p->ctx.count--;
  p->ctx.evictions++;

  if (p->ctx.count == 0)
    p->ctx.head = p->ctx.tail = p->ctx.used = 0;
}

// 把 16 页上下文区映射到 pagetable。（只建映射，不改 p->ctx 的记账。）
int
agent_ctx_map(pagetable_t pagetable, uint64 base)
{
  uint64 a;

  for (a = base; a < base + AGENT_CTX_SIZE; a += PGSIZE) {
    char *mem = kalloc_clean();

    if (mem == 0) {
      uvmunmap(pagetable, base, (a - base) / PGSIZE, 1);
      return -1;
    }
    // 只给 R|U：用户态读得到、写不了；内核走物理直映写入。
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U) != 0) {
      kfree(mem);
      uvmunmap(pagetable, base, (a - base) / PGSIZE, 1);
      return -1;
    }
  }
  return 0;
}

// 把记账复位成"空区"。
void
agent_ctx_reset(struct proc *p)
{
  p->ctx.base = AGENT_CTX_BASE;
  p->ctx.size = AGENT_CTX_SIZE;
  p->ctx.used = 0;
  p->ctx.head = 0;
  p->ctx.tail = 0;
  p->ctx.count = 0;
  p->ctx.evictions = 0;
}

// 映射 + 复位。创建 Agent 进程、exec 之后重建都走这里。
int
agent_ctx_alloc(struct proc *p, pagetable_t pagetable)
{
  if (agent_ctx_map(pagetable, AGENT_CTX_BASE) != 0)
    return -1;
  agent_ctx_reset(p);
  return 0;
}

// 归还上下文区的 16 个物理页，并把记账清空。
void
agent_ctx_release(struct proc *p, pagetable_t pagetable)
{
  if (pagetable && p->ctx.size != 0)
    uvmunmap(pagetable, p->ctx.base, p->ctx.size / PGSIZE, 1);
  memset(&p->ctx, 0, sizeof(p->ctx));
}

// 分配进程时初始化 5 个 Agent 字段。
void
agent_pcb_init(struct proc *p)
{
  p->agent_type = 0;
  p->heartbeat_interval = 0;
  p->resource_quota = AGENT_CTX_SIZE;
  p->loop_state = LOOP_IDLE;
  memset(&p->ctx, 0, sizeof(p->ctx));
}

// ---------- 任务一：Agent 进程 ----------

// sys_agent_create(type, interval, quota) -> 0 | 负错误码
//
// v0 语义：把调用进程本身升级为 Agent，返回 0。
//   （"创建新进程"需要动 fork 路径，v1 再补；接口与错误码不变，用户态不需要改。）
uint64
sys_agent_create(void)
{
  int type, interval;
  uint64 quota;
  struct proc *p = myproc();

  argint(0, &type);
  argint(1, &interval);
  argaddr(2, &quota);

  if (type <= 0 || type > 255)
    return AGENT_E_ARG;
  if (interval < 0)
    return AGENT_E_ARG;
  if (quota == 0 || quota > AGENT_CTX_SIZE)
    quota = AGENT_CTX_SIZE;

  if (p->agent_type != 0)
    return AGENT_E_BUSY;

  if (agent_ctx_alloc(p, p->pagetable) != 0)
    return AGENT_E_QUOTA;

  p->agent_type = type;
  p->heartbeat_interval = interval;
  p->resource_quota = quota;
  p->loop_state = LOOP_IDLE;

  return AGENT_OK;
}

// sys_agent_info(pid, struct agent_info *) -> 0 | 负错误码
//   pid <= 0 或等于自己：查自己；否则按 pid 查进程表。
uint64
sys_agent_info(void)
{
  int pid;
  uint64 ubuf;
  struct agent_info info;
  struct proc *p = myproc(), *t = 0;

  argint(0, &pid);
  argaddr(1, &ubuf);

  if (pid <= 0 || pid == p->pid) {
    t = p;
  } else {
    for (int i = 0; i < NPROC; i++) {
      if (proc[i].pid == pid && proc[i].state != UNUSED) {
        t = &proc[i];
        break;
      }
    }
    if (t == 0)
      return AGENT_E_ARG;
  }

  memset(&info, 0, sizeof(info));
  acquire(&t->lock);
  info.type = t->agent_type;
  info.heartbeat_interval = t->heartbeat_interval;
  info.quota = t->resource_quota;
  info.loop_state = t->loop_state;
  info.base = t->ctx.base;
  info.size = t->ctx.size;
  info.used = t->ctx.used;
  info.count = t->ctx.count;
  info.evictions = t->ctx.evictions;
  release(&t->lock);

  info.free_pages = kalloc_freepages();

  if (t->agent_type == 0)
    return AGENT_E_ARG;
  if (copyout(p->pagetable, p->sz, ubuf, (char *)&info, sizeof(info)) < 0)
    return AGENT_E_FAULT;

  return AGENT_OK;
}

// ---------- 任务三：上下文路径管理（机制部分） ----------

// sys_context_push(data, len) -> 0 | 负错误码
uint64
sys_context_push(void)
{
  uint64 data;
  int len;
  uint32 need, hdr[2], quota;
  struct proc *p = myproc();

  argaddr(0, &data);
  argint(1, &len);

  if (p->agent_type == 0)
    return AGENT_E_ARG;
  if (len <= 0 || len > AGENT_MAX_PAYLOAD)
    return AGENT_E_ARG;

  need = CTX_HDR + ctx_pad((uint32)len);
  quota = (p->resource_quota == 0 || p->resource_quota > p->ctx.size)
              ? p->ctx.size
              : (uint32)p->resource_quota;
  if (need > quota)
    return AGENT_E_ARG;

  // 配额淘汰：丢最老的，直到装得下
  while (p->ctx.used + need > quota && p->ctx.count > 0)
    ctx_evict_head(p);
  if (p->ctx.used + need > quota)
    return AGENT_E_QUOTA;

  // 区内空间不够：先惰性压缩一次
  if (p->ctx.tail + need > p->ctx.size) {
    if (ctx_compact(p) != 0)
      return AGENT_E_IO;
    if (p->ctx.tail + need > p->ctx.size)
      return AGENT_E_QUOTA;
  }

  hdr[0] = (uint32)len;
  hdr[1] = 0;
  if (ctx_put(p, p->ctx.tail, (char *)hdr, CTX_HDR) != 0)
    return AGENT_E_FAULT;
  if (ctx_put_user(p, p->ctx.tail + CTX_HDR, data, (uint32)len) != 0)
    return AGENT_E_FAULT;

  p->ctx.tail += need;
  p->ctx.used = p->ctx.tail - p->ctx.head;
  p->ctx.count++;

  return AGENT_OK;
}

// sys_context_query(idx, buf, buflen) -> 节点长度 | 负错误码
//   idx 从 0 开始（0 = 最老）。buflen 必须 >= 节点长度。
uint64
sys_context_query(void)
{
  int idx, buflen;
  uint64 ubuf;
  uint32 off, hdr[2];
  struct proc *p = myproc();

  argint(0, &idx);
  argaddr(1, &ubuf);
  argint(2, &buflen);

  if (p->agent_type == 0)
    return AGENT_E_ARG;
  if (idx < 0 || (uint32)idx >= p->ctx.count)
    return AGENT_E_ARG;

  off = p->ctx.head;
  for (int i = 0; i < idx; i++) {
    if (ctx_get(p, off, (char *)hdr, CTX_HDR) != 0)
      return AGENT_E_FAULT;
    off += CTX_HDR + ctx_pad(hdr[0]);
  }

  if (ctx_get(p, off, (char *)hdr, CTX_HDR) != 0)
    return AGENT_E_FAULT;
  if (hdr[0] > (uint32)buflen)
    return AGENT_E_ARG;
  if (ctx_get_user(p, off + CTX_HDR, ubuf, hdr[0]) != 0)
    return AGENT_E_FAULT;

  return hdr[0];
}

// sys_context_rollback(n) -> 0 | 负错误码
//   从尾部回退 n 个节点，只动指针，不动物理页。
uint64
sys_context_rollback(void)
{
  int n;
  uint32 off, hdr[2], keep;
  struct proc *p = myproc();

  argint(0, &n);

  if (p->agent_type == 0)
    return AGENT_E_ARG;
  if (n < 0 || (uint32)n > p->ctx.count)
    return AGENT_E_ARG;

  keep = p->ctx.count - (uint32)n;
  off = p->ctx.head;
  for (uint32 i = 0; i < keep; i++) {
    if (ctx_get(p, off, (char *)hdr, CTX_HDR) != 0)
      return AGENT_E_FAULT;
    off += CTX_HDR + ctx_pad(hdr[0]);
  }

  p->ctx.tail = off;
  p->ctx.used = off - p->ctx.head;
  p->ctx.count = keep;
  if (keep == 0)
    p->ctx.head = p->ctx.tail = p->ctx.used = 0;

  return AGENT_OK;
}

// sys_context_clear() -> 0
uint64
sys_context_clear(void)
{
  struct proc *p = myproc();

  if (p->agent_type == 0)
    return AGENT_E_ARG;

  p->ctx.head = 0;
  p->ctx.tail = 0;
  p->ctx.used = 0;
  p->ctx.count = 0;
  p->ctx.evictions = 0;

  return AGENT_OK;
}
