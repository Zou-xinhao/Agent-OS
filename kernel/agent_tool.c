#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "agent_proto.h"

extern struct proc proc[NPROC];

_Static_assert(sizeof(struct agent_req) == 12, "Agent request ABI");
_Static_assert(sizeof(struct agent_resp) == 12, "Agent response ABI");
_Static_assert(sizeof(struct agent_resp) +
                   NPROC * sizeof(struct agent_proc_row) <=
                 PGSIZE,
               "Process query must fit in one page");
_Static_assert(AGENT_MAX_PAYLOAD <= PGSIZE,
               "Tool request must fit in one page");

static const char tool_list[] =
  "1 query_process status?:used|sleeping|runnable|running|zombie "
  "type?:ordinary|agent\n"
  "3 read_context index:uint\n"
  "5 get_system_status\n";

struct tool_args {
  int state;
  int type;
  int index;
};

static const uint32 wire_state[] = {
  [USED] = AGENT_PROC_USED,         [SLEEPING] = AGENT_PROC_SLEEPING,
  [RUNNABLE] = AGENT_PROC_RUNNABLE, [RUNNING] = AGENT_PROC_RUNNING,
  [ZOMBIE] = AGENT_PROC_ZOMBIE,
};

static const char *const state_names[] = {
  [AGENT_PROC_USED] = "used",         [AGENT_PROC_SLEEPING] = "sleeping",
  [AGENT_PROC_RUNNABLE] = "runnable", [AGENT_PROC_RUNNING] = "running",
  [AGENT_PROC_ZOMBIE] = "zombie",
};

static int
matches(const char *bytes, uint len, const char *s)
{
  return len == strlen(s) && memcmp(bytes, s, len) == 0;
}

static int
parse_state(const char *value, uint len)
{
  for (int i = AGENT_PROC_USED; i < AGENT_PROC_STATE_COUNT; i++)
    if (matches(value, len, state_names[i]))
      return i;
  return -1;
}

static int
parse_index(const char *value, uint len)
{
  int index = 0;

  if (len == 0 || len > 10)
    return -1;
  for (uint i = 0; i < len; i++) {
    int digit = value[i] - '0';

    if (digit < 0 || digit > 9 || index > (2147483647 - digit) / 10)
      return -1;
    index = index * 10 + digit;
  }
  return index;
}

static int
parse_param(int tool, struct tool_args *args, const char *key, uint klen,
            const char *value, uint vlen)
{
  if (tool == TOOL_QUERY_PROCESS) {
    if (args->state < 0 && matches(key, klen, "status")) {
      args->state = parse_state(value, vlen);
      return args->state < 0 ? AGENT_E_ARG : AGENT_OK;
    }
    if (args->type < 0 && matches(key, klen, "type")) {
      if (matches(value, vlen, "ordinary"))
        args->type = AGENT_TYPE_NORMAL;
      else if (matches(value, vlen, "agent"))
        args->type = AGENT_TYPE_AGENT;
      else
        return AGENT_E_ARG;
      return AGENT_OK;
    }
  }
  if (tool == TOOL_READ_CONTEXT && args->index < 0 &&
      matches(key, klen, "index")) {
    args->index = parse_index(value, vlen);
    return args->index < 0 ? AGENT_E_ARG : AGENT_OK;
  }
  return AGENT_E_ARG;
}

static int
parse_params(const struct agent_req *req, const char *payload,
             struct tool_args *args)
{
  uint offset = req->argc * sizeof(struct agent_kv);

  if (offset > req->payload_len)
    return AGENT_E_ARG;
  for (uint i = 0; i < req->argc; i++) {
    struct agent_kv kv;

    memmove(&kv, payload + i * sizeof(kv), sizeof(kv));
    if (kv.klen == 0 || kv.klen + kv.vlen > req->payload_len - offset)
      return AGENT_E_ARG;
    if (parse_param(req->tool_id, args, payload + offset, kv.klen,
                    payload + offset + kv.klen, kv.vlen) < 0)
      return AGENT_E_ARG;
    offset += kv.klen + kv.vlen;
  }
  if (offset != req->payload_len ||
      (req->tool_id == TOOL_READ_CONTEXT && args->index < 0))
    return AGENT_E_ARG;
  return AGENT_OK;
}

static uint
query_process(const struct tool_args *args, struct agent_proc_row *rows)
{
  uint count = 0;
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    int state = wire_state[p->state];
    if (p->state != UNUSED && (args->state < 0 || args->state == state) &&
        (args->type < 0 || args->type == p->agent.type)) {
      struct agent_proc_row *row = &rows[count++];

      memset(row, 0, sizeof(*row));
      row->pid = p->pid;
      row->state = state;
      row->type = p->agent.type;
      safestrcpy(row->name, p->name, sizeof(row->name));
    }
    release(&p->lock);
  }
  return count * sizeof(*rows);
}

static void
system_status(struct agent_system_status *status)
{
  memset(status, 0, sizeof(*status));
  acquire(&tickslock);
  status->uptime_ticks = ticks;
  release(&tickslock);
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state != UNUSED)
      status->processes[wire_state[p->state]]++;
    release(&p->lock);
  }
}

static int
user_range(uint64 addr, uint len)
{
  return addr != 0 && addr < MAXVA && len <= MAXVA - addr;
}

static int
execute_tool(struct proc *p, uint64 addr, int len, char **scratch,
             uint32 *data_len)
{
  struct agent_req req;
  struct tool_args args = {.state = -1, .type = -1, .index = -1};

  if (p->agent.type != AGENT_TYPE_AGENT || len < (int)sizeof(req))
    return AGENT_E_ARG;
  if (!user_range(addr, len) ||
      copyin(p->pagetable, p->sz, (char *)&req, addr, sizeof(req)) < 0)
    return AGENT_E_FAULT;
  if (req.magic != AGENT_MAGIC || req.flags != 0 ||
      req.payload_len > AGENT_MAX_PAYLOAD ||
      req.payload_len != len - sizeof(req) || req.argc > 2)
    return AGENT_E_ARG;
  if (req.tool_id != TOOL_QUERY_PROCESS && req.tool_id != TOOL_READ_CONTEXT &&
      req.tool_id != TOOL_GET_SYSTEM_STATUS)
    return AGENT_E_TOOL;

  *scratch = kalloc();
  if (*scratch == 0)
    return AGENT_E_BUSY;
  if (copyin(p->pagetable, p->sz, *scratch, addr + sizeof(req),
             req.payload_len) < 0)
    return AGENT_E_FAULT;
  if (parse_params(&req, *scratch, &args) < 0)
    return AGENT_E_ARG;

  char *data = *scratch + sizeof(struct agent_resp);
  switch (req.tool_id) {
  case TOOL_QUERY_PROCESS:
    *data_len = query_process(&args, (struct agent_proc_row *)data);
    break;
  case TOOL_READ_CONTEXT: {
    // 线上数据从页内偏移 12 字节处开始，故改用对齐的局部变量。
    struct agent_record record;
    int result = agent_read_record(args.index, &record);
    if (result < 0)
      return result;
    memmove(data, &record, sizeof(record));
    *data_len = sizeof(record);
    break;
  }
  case TOOL_GET_SYSTEM_STATUS:
    system_status((struct agent_system_status *)data);
    *data_len = sizeof(struct agent_system_status);
    break;
  }
  return AGENT_OK;
}

uint64
sys_tool_call(void)
{
  struct proc *p = myproc();
  uint64 req_addr, resp_addr;
  int req_len, resp_len;
  char *scratch = 0;
  struct agent_resp resp = {.magic = AGENT_MAGIC};

  argaddr(0, &req_addr);
  argint(1, &req_len);
  argaddr(2, &resp_addr);
  argint(3, &resp_len);
  if (resp_len < 0)
    return AGENT_E_ARG;
  if (resp_len < sizeof(resp))
    return AGENT_E_QUOTA;
  if (!user_range(resp_addr, resp_len))
    return AGENT_E_FAULT;

  resp.status = execute_tool(p, req_addr, req_len, &scratch, &resp.data_len);
  if (resp.status == AGENT_OK && resp.data_len > resp_len - sizeof(resp))
    resp.status = AGENT_E_QUOTA;
  if (resp.status < 0)
    resp.data_len = 0;

  char *out = (char *)&resp;
  if (resp.data_len != 0) {
    memmove(scratch, &resp, sizeof(resp));
    out = scratch;
  }
  int result = resp.status;
  if (copyout(p->pagetable, p->sz, resp_addr, out,
              sizeof(resp) + resp.data_len) < 0)
    result = AGENT_E_FAULT;
  if (scratch != 0)
    kfree(scratch);
  return result;
}

uint64
sys_tool_list(void)
{
  struct proc *p = myproc();
  uint64 addr;
  int len;

  argaddr(0, &addr);
  argint(1, &len);
  if (p->agent.type != AGENT_TYPE_AGENT || len < 0)
    return AGENT_E_ARG;
  if (addr == 0 && len == 0)
    return sizeof(tool_list) - 1;
  if (len < sizeof(tool_list) - 1)
    return AGENT_E_QUOTA;
  if (!user_range(addr, len) ||
      copyout(p->pagetable, p->sz, addr, (char *)tool_list,
              sizeof(tool_list) - 1) < 0)
    return AGENT_E_FAULT;
  return sizeof(tool_list) - 1;
}

// ---------- 任务四：文件系统属性（加分项） ----------

// sys_file_setattr(path, key, val) -> 0 | 负错误码
uint64
sys_file_setattr(void)
{
  return AGENT_E_NOSYS;
}

// sys_file_getattr(path, key, buf, len) -> 值长度 | 负错误码
uint64
sys_file_getattr(void)
{
  return AGENT_E_NOSYS;
}

// sys_file_query(key, val, out, outlen) -> 命中条数 | 负错误码
uint64
sys_file_query(void)
{
  return AGENT_E_NOSYS;
}
