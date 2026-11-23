#include "kernel/types.h"
#include "kernel/agent_proto.h"
#include "user/user.h"

// Small check that the Agent syscall entries still dispatch from user space.
int
main(void)
{
  int ok = agent_create(0, 0, 4096) == -1 && tool_list(0, 0) == AGENT_E_ARG &&
           context_clear() == AGENT_E_ARG &&
           agent_heartbeat_stop() == AGENT_E_NOSYS;

  printf("t_stub: %s\n", ok ? "PASS" : "FAIL");
  exit(ok ? 0 : 1);
}
