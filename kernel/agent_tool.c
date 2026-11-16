//
// 工具调用与文件系统属性扩展
// 赛题：任务二（结构化交互接口）、任务四（文件系统扩展，加分项）
// 负责人：B
//
// 本文件由 A 在铺桩阶段建立，函数体由 B 在自己的分支里替换为真实实现。
// A 不要在此文件里写逻辑，避免两人同时改同一个文件。
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "agent_proto.h"

// ---------- 任务二：结构化交互接口 ----------

// sys_tool_call(req, reqlen, resp, resplen) -> 0 | 负错误码
uint64
sys_tool_call(void)
{
  return AGENT_E_NOSYS;
}

// sys_tool_list(buf, buflen) -> 写入字节数
uint64
sys_tool_list(void)
{
  return AGENT_E_NOSYS;
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
