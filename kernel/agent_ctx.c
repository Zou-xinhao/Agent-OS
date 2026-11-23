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

// Only the running process can change its record data. Other processes
// may snapshot its metadata under p->lock, but cannot access its records.
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
  return AGENT_E_NOSYS;
}

// sys_agent_heartbeat_stop() -> 0
uint64
sys_agent_heartbeat_stop(void)
{
  return AGENT_E_NOSYS;
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
  return AGENT_E_NOSYS;
}

// sys_agent_unwatch(watch_id) -> 0 | -1
uint64
sys_agent_unwatch(void)
{
  return AGENT_E_NOSYS;
}
