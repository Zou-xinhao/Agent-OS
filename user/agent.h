// 用户态看到的 Agent 结构（模块 A 维护）。
// 只 include 这一个头，不要把 kernel/proc.h 那种内核私有结构带进用户态。
#ifndef USER_AGENT_H
#define USER_AGENT_H

#include "kernel/types.h"
#include "kernel/agent_proto.h"

#endif
