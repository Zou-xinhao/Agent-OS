//
// Agent 进程与上下文管理
// 赛题：任务一（Agent 进程与地址空间）、任务三（上下文路径管理）、任务五（Agent Loop）
// 负责人：A
//
// 当前状态：桩实现，全部返回 -1（未实现）。
// 填实现时只替换函数体，不要改函数名与签名——B/C 的代码依赖这些符号。
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// ---------- 任务一：Agent 进程 ----------

// sys_agent_create(type, interval, quota) -> pid | -1
uint64
sys_agent_create(void)
{
  return -1;
}

// sys_agent_info(pid, struct agent_info *) -> 0 | -1
uint64
sys_agent_info(void)
{
  return -1;
}

// ---------- 任务三：上下文路径管理 ----------

// sys_context_push(data, len) -> 0 | 负错误码
uint64
sys_context_push(void)
{
  return -1;
}

// sys_context_query(idx, buf, buflen) -> 节点长度 | 负错误码
uint64
sys_context_query(void)
{
  return -1;
}

// sys_context_rollback(n) -> 0 | -1
uint64
sys_context_rollback(void)
{
  return -1;
}

// sys_context_clear() -> 0
uint64
sys_context_clear(void)
{
  return -1;
}

// ---------- 任务五：Agent Loop 机制 ----------

// sys_agent_heartbeat_set(interval) -> 0 | -1
uint64
sys_agent_heartbeat_set(void)
{
  return -1;
}

// sys_agent_heartbeat_stop() -> 0
uint64
sys_agent_heartbeat_stop(void)
{
  return -1;
}

// sys_agent_watch(event_id) -> watch_id | -1
uint64
sys_agent_watch(void)
{
  return -1;
}

// sys_agent_wait(timeout) -> event_id | -1（超时）
uint64
sys_agent_wait(void)
{
  return -1;
}

// sys_agent_unwatch(watch_id) -> 0 | -1
uint64
sys_agent_unwatch(void)
{
  return -1;
}
