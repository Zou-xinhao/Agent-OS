#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "agent_proto.h"

_Static_assert(sizeof(struct agent_record) == AGENT_CONTEXT_RECORD,
               "Agent Context record ABI");

// 模块 A（内存与地址空间）移植要点：
//   1) Context 区不再在 kagent_fork 时一次性 uvmalloc(quota)，
//      改为"预留 + 惰性批量提交"：第一次访问某页时才映射，
//      一次映射 4 页（16 KiB），与企划书 4.5 的方案一致。
//   2) 页仍然给 PTE_W，因为 ABI 要求用户程序可以直接读自己的 Context 区
//      （user/agenttests.c 会解引用 context_base），同时 copyout 写用户页
//      在 vm.c 里也要求 PTE_W。
//   3) 分配走 kalloc_clean()（per-CPU 缓存 + 清零语义），页表页同理。

#define CTX_COMMIT_PAGES 4

uint64
sys_agent_create(void)
{
  int type, interval;
  uint64 quota;

  argint(0, &type);
  argint(1, &interval);
  argaddr(2, &quota);
  return kagent_fork(type, interval, quota);
}

uint64
sys_agent_info(void)
{
  struct proc *p = myproc();
  struct agent_info info;
  int pid;
  uint64 dst;

  argint(0, &pid);
  argaddr(1, &dst);
  if (agent_get_info(pid, &info) < 0)
    return -1;
  return copyout(p->pagetable, p->sz, dst, (char *)&info, sizeof(info));
}

static uint
context_capacity(struct proc *p)
{
  return p->agent.quota / sizeof(struct agent_record);
}

static uint64
record_addr(struct proc *p, uint slot)
{
  return p->agent.context_base + slot * sizeof(struct agent_record);
}

// 确保 va 所在页已映射；顺带把同批的 4 页一起提交（首次触碰该批即整批映射）。
// 返回 0 表示 va 所在页可用；-1 表示配额或物理内存不足。
// 只能由当前进程修改自己的页表。此函数不会睡眠。
static int
ctx_commit(struct proc *p, uint64 va)
{
  uint64 base = p->agent.context_base;
  uint64 limit = base + p->agent.quota;
  uint64 a = PGROUNDDOWN(va);
  uint64 end;
  int ok = 0;

  if (va < base || va >= limit)
    return -1;

  end = a + CTX_COMMIT_PAGES * PGSIZE;
  if (end > limit)
    end = limit;

  for (uint64 q = a; q < end; q += PGSIZE) {
    pte_t *pte = walk(p->pagetable, q, 0);
    char *mem;

    if (pte && (*pte & PTE_V)) {
      if (q == a)
        ok = 1;
      continue;
    }
    mem = kalloc_clean();
    if (mem == 0)
      break;
    if (mappages(p->pagetable, q, PGSIZE, (uint64)mem, PTE_R | PTE_U | PTE_W) !=
        0) {
      kfree(mem);
      break;
    }
    if (q == a)
      ok = 1;
  }
  return ok ? 0 : -1;
}

// 处理当前 Agent 对已预留但尚未提交的 Context 页的首次访问。
uint64
agent_context_fault(struct proc *p, uint64 va)
{
  if (p->agent.type != AGENT_TYPE_AGENT || va < p->agent.context_base ||
      va - p->agent.context_base >= p->agent.quota ||
      ismapped(p->pagetable, va))
    return 0;
  if (ctx_commit(p, va) < 0)
    return 0;
  return walkaddr(p->pagetable, va);
}

// 只允许当前进程改自己的记录；别的进程只能在 p->lock 下快照元数据。
int
agent_read_record(int index, struct agent_record *record)
{
  struct proc *p = myproc();
  uint slot, len;

  acquire(&p->lock);
  if (p->agent.type != AGENT_TYPE_AGENT || index < 0 ||
      (uint)index >= p->agent.count) {
    release(&p->lock);
    return AGENT_E_ARG;
  }
  slot = (p->agent.head + index) % context_capacity(p);
  len = p->agent_lengths[slot];
  release(&p->lock);

  if (copyin(p->pagetable, p->sz, (char *)record, record_addr(p, slot),
             sizeof(*record)) < 0)
    return AGENT_E_FAULT;
  record->len = len;
  record->reserved = 0;
  memset(record->data + len, 0, sizeof(record->data) - len);
  return AGENT_OK;
}

uint64
sys_context_push(void)
{
  struct proc *p = myproc();
  struct agent_record record;
  uint64 src;
  int len;
  uint slot, capacity;

  argaddr(0, &src);
  argint(1, &len);
  if (p->agent.type != AGENT_TYPE_AGENT || len <= 0)
    return AGENT_E_ARG;
  if (len > AGENT_CONTEXT_DATA)
    return AGENT_E_QUOTA;

  memset(&record, 0, sizeof(record));
  if (copyin(p->pagetable, p->sz, (char *)record.data, src, len) < 0)
    return AGENT_E_FAULT;
  acquire(&tickslock);
  record.tick = ticks;
  release(&tickslock);
  record.len = len;

  acquire(&p->lock);
  capacity = context_capacity(p);
  slot = (p->agent.head + p->agent.count) % capacity;
  if (ctx_commit(p, record_addr(p, slot)) < 0) {
    release(&p->lock);
    return AGENT_E_QUOTA;
  }
  if (copyout(p->pagetable, p->sz, record_addr(p, slot), (char *)&record,
              sizeof(record)) < 0) {
    release(&p->lock);
    return AGENT_E_FAULT;
  }
  p->agent_lengths[slot] = len;
  if (p->agent.count == capacity)
    p->agent.head = (p->agent.head + 1) % capacity;
  else
    p->agent.count++;
  release(&p->lock);
  return AGENT_OK;
}

uint64
sys_context_query(void)
{
  struct proc *p = myproc();
  struct agent_record record;
  int index, buflen, status;
  uint64 dst;

  argint(0, &index);
  argaddr(1, &dst);
  argint(2, &buflen);
  if (buflen < 0)
    return AGENT_E_ARG;
  status = agent_read_record(index, &record);
  if (status < 0)
    return status;
  if ((uint)buflen < record.len)
    return AGENT_E_QUOTA;
  if (copyout(p->pagetable, p->sz, dst, (char *)record.data, record.len) < 0)
    return AGENT_E_FAULT;
  return record.len;
}

uint64
sys_context_rollback(void)
{
  struct proc *p = myproc();
  int n;

  argint(0, &n);
  acquire(&p->lock);
  if (p->agent.type != AGENT_TYPE_AGENT || n < 0 || (uint)n > p->agent.count) {
    release(&p->lock);
    return AGENT_E_ARG;
  }
  p->agent.count -= n;
  if (p->agent.count == 0)
    p->agent.head = 0;
  release(&p->lock);
  return AGENT_OK;
}

uint64
sys_context_clear(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->agent.type != AGENT_TYPE_AGENT) {
    release(&p->lock);
    return AGENT_E_ARG;
  }
  p->agent.head = 0;
  p->agent.count = 0;
  release(&p->lock);
  return AGENT_OK;
}

// ---------- 任务五：Agent Loop 机制 ----------

// sys_agent_heartbeat_set(interval) -> 0 | -1
uint64
sys_agent_heartbeat_set(void)
{
  int interval;

  argint(0, &interval);
  return interval > 0 ? agent_heartbeat_configure(interval) : -1;
}

// sys_agent_heartbeat_stop() -> 0
uint64
sys_agent_heartbeat_stop(void)
{
  return agent_heartbeat_configure(0);
}

// sys_agent_watch(event_id) -> watch_id | -1
uint64
sys_agent_watch(void)
{
  return AGENT_E_NOSYS;
}

// sys_agent_wait(timeout) -> event_id | -1（超时）
uint64
sys_agent_wait(void)
{
  int timeout;

  argint(0, &timeout);
  return agent_wait_event(timeout);
}

// sys_agent_unwatch(watch_id) -> 0 | -1
uint64
sys_agent_unwatch(void)
{
  return AGENT_E_NOSYS;
}
