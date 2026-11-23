#ifndef AGENT_ABI_H
#define AGENT_ABI_H

// Shared with user programs. Include kernel/types.h first.
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

// Process query returns a packed array of agent_proc_row. Its length is
// given by the response header; an empty query result is valid.
struct agent_system_status {
  uint32 uptime_ticks;
  uint32 processes[AGENT_PROC_STATE_COUNT]; // element 0 is reserved
};

#endif
