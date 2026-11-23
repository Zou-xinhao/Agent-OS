#ifndef AGENT_ABI_H
#define AGENT_ABI_H

// 与用户程序共享；包含本文件前先包含 kernel/types.h。
#define AGENT_TYPE_NORMAL 0
#define AGENT_TYPE_AGENT  1
#define AGENT_LOOP_IDLE   0

#define AGENT_MAX_QUOTA      65536
#define AGENT_CONTEXT_RECORD 256
#define AGENT_CONTEXT_DATA   240
#define AGENT_CONTEXT_SLOTS  (AGENT_MAX_QUOTA / AGENT_CONTEXT_RECORD)

struct agent_info {
  int pid;
  int type;
  int heartbeat_interval;
  int loop_state;
  uint64 context_base;
  uint64 quota;
  uint32 head;
  uint32 count;
};

struct agent_record {
  uint64 tick;
  uint32 len;
  uint32 reserved;
  uint8 data[AGENT_CONTEXT_DATA];
};

enum agent_proc_state {
  AGENT_PROC_USED = 1,
  AGENT_PROC_SLEEPING,
  AGENT_PROC_RUNNABLE,
  AGENT_PROC_RUNNING,
  AGENT_PROC_ZOMBIE,
  AGENT_PROC_STATE_COUNT,
};

struct agent_proc_row {
  int pid;
  uint32 state;
  uint32 type;
  char name[16];
};

// 进程查询返回紧凑排列的 agent_proc_row 数组，其长度由应答头给出；
// 查询结果为空是合法的。
struct agent_system_status {
  uint32 uptime_ticks;
  uint32 processes[AGENT_PROC_STATE_COUNT]; // 元素 0 保留
};

#endif
