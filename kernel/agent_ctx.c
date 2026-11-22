//
// Agent Loop（赛题任务五）
// 负责人：A（机制）/ C（场景与验证）
//
// 当前状态：桩实现，全部返回 -1（未实现）。
// 填实现时只替换函数体，不要改函数名与签名——B/C 的代码依赖这些符号。
//
// 注意：任务一（Agent 进程）与任务三的 Context 内存机制已经搬到 agent_mem.c，
// 本文件只保留任务五的心跳与事件接口，避免两个模块抢同一个文件。
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "agent_proto.h"

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
