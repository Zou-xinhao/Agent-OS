// 模块 A 的基础演示：启动 Agent 子进程，展示 Context 直读和 FIFO 淘汰。
#include "user/agent.h"
#include "user/user.h"

static void
check(int condition, const char *what)
{
  if (!condition) {
    fprintf(2, "agent_demo: FAIL (%s)\n", what);
    exit(1);
  }
}

int
main(void)
{
  struct agent_info info;
  int status;
  int pid = agent_create(AGENT_TYPE_AGENT, 10, 8192);

  check(pid >= 0, "agent_create");
  if (pid > 0) {
    check(wait(&status) == pid && status == 0, "wait for Agent");
    printf("agent_demo: PASS\n");
    exit(0);
  }

  check(agent_info(getpid(), &info) == 0, "agent_info");
  check(info.type == AGENT_TYPE_AGENT && info.quota == 8192 && info.count == 0,
        "initial metadata");
  printf("agent_demo: pid=%d quota=%d bytes heartbeat=%d ticks\n", info.pid,
         (int)info.quota, info.heartbeat_interval);

  int capacity = info.quota / sizeof(struct agent_record);
  for (int i = 0; i < capacity + 4; i++)
    check(context_push(&i, sizeof(i)) == 0, "context_push");

  int oldest;
  check(agent_info(getpid(), &info) == 0 && info.count == capacity,
        "bounded FIFO");
  check(context_query(0, &oldest, sizeof(oldest)) == sizeof(oldest) &&
          oldest == 4,
        "oldest remaining record");
  struct agent_record *records = (struct agent_record *)info.context_base;
  int direct;
  memmove(&direct, records[info.head].data, sizeof(direct));
  check(direct == oldest, "direct read");
  printf("agent_demo: slots=%d oldest=%d after four evictions\n", capacity,
         oldest);

  check(context_rollback(1) == 0, "context_rollback");
  check(agent_info(getpid(), &info) == 0 && info.count == capacity - 1,
        "rollback count");
  check(context_clear() == 0 && agent_info(getpid(), &info) == 0 &&
          info.count == 0,
        "context_clear");
  exit(0);
}
