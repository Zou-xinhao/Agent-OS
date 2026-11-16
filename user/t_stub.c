// 冒烟测试：验证新增的系统调用桩已经通到用户态
// 全部返回负数（未实现）+ uptime 正常，就说明 ecall -> usertrap -> syscall() 分发 这条链路是通的。
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  int a, b, c, d;
  uint t;

  a = agent_create(1, 10, 65536);
  b = tool_list(0, 0);
  c = context_clear();
  d = agent_heartbeat_stop();
  t = uptime();

  printf("t_stub: agent_create    -> %d\n", a);
  printf("t_stub: tool_list       -> %d\n", b);
  printf("t_stub: context_clear   -> %d\n", c);
  printf("t_stub: heartbeat_stop  -> %d\n", d);
  printf("t_stub: uptime          -> %d\n", t);

  if (a <= 0 && b <= 0 && c <= 0 && d <= 0 && t > 0)
    printf("t_stub: PASS (pipeline ok)\n");
  else
    printf("t_stub: FAIL\n");

  exit(0);
}
