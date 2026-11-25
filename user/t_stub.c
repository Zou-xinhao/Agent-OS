#include "kernel/types.h"
#include "kernel/agent_proto.h"
#include "user/user.h"

// 小检查：确认 Agent 系统调用入口仍能从用户态分发。
int
main(void)
{
  int ok = agent_create(0, 0, 4096) == -1 && tool_list(0, 0) == AGENT_E_ARG &&
           context_clear() == AGENT_E_ARG && agent_heartbeat_stop() == -1;

  printf("t_stub: %s\n", ok ? "PASS" : "FAIL");
  exit(ok ? 0 : 1);
}
