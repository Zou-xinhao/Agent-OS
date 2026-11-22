// 模块 A 骨架演示 + 自检：Agent 进程字段、Context 区映射、FIFO 追加、
// 按序号取、回滚、清空、配额淘汰。
// 当自检全部通过时最后一行打印 "agent_demo: PASS"。

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/agent.h"

static int fails;

static void
check(int cond, char *what)
{
  if (cond)
    printf("  [ok]   %s\n", what);
  else {
    printf("  [FAIL] %s\n", what);
    fails++;
  }
}

int
main(void)
{
  static char big[1000];
  struct agent_info info;
  char buf[1200];
  int r, i;

  memset(big, 'A', sizeof(big));
  for (i = 0; i < (int)sizeof(big); i++)
    big[i] = (char)('a' + (i % 26));

  printf("== agent_demo: 内存与地址空间骨架 ==\n");

  // 1. 升级为 Agent：类型 1、心跳 10 tick、配额 8 KiB
  r = agent_create(1, 10, 8192);
  check(r == 0, "agent_create 返回 0");

  // 2. PCB 字段与 Context 区布局
  r = agent_info(0, &info);
  check(r == 0, "agent_info 返回 0");
  check(info.type == 1, "PCB agent_type == 1");
  check(info.heartbeat_interval == 10, "PCB heartbeat_interval == 10");
  check(info.quota == 8192, "PCB resource_quota == 8192");
  check(info.loop_state == 0, "PCB loop_state == idle");
  check(info.size == 16 * 4096, "Context 区 16 页 = 64 KiB");
  check(info.base % 4096 == 0 && info.base > 0, "Context 区按页对齐");
  check(info.used == 0 && info.count == 0, "Context 区初始为空");
  check(info.free_pages > 0, "分配器有空闲页");
  printf("  ctx: base=0x%lx size=%d quota=%ld free=%ld\n", (uint64)info.base,
         (int)info.size, (long)info.quota, (long)info.free_pages);

  // 3. 追加 3 个节点
  check(context_push("hello agent", 12) == 0, "push 12 B 节点");
  check(context_push(big, sizeof(big)) == 0, "push 1000 B 节点");
  check(context_push("tail", 5) == 0, "push 5 B 节点");
  r = agent_info(0, &info);
  check(r == 0 && info.count == 3, "count == 3");
  check(info.used > 0, "used > 0");

  // 4. 按序号取（0 = 最老）
  memset(buf, 0, sizeof(buf));
  r = context_query(0, buf, sizeof(buf));
  check(r == 12 && memcmp(buf, "hello agent", 11) == 0, "query(0) 内容正确");

  memset(buf, 0, sizeof(buf));
  r = context_query(1, buf, sizeof(buf));
  check(r == (int)sizeof(big) && memcmp(buf, big, sizeof(big)) == 0,
        "query(1) 1000 B 内容逐字节一致");

  memset(buf, 0, sizeof(buf));
  r = context_query(2, buf, sizeof(buf));
  check(r == 5 && memcmp(buf, "tail", 4) == 0, "query(2) 内容正确");

  // 5. 越界序号与过小的接收缓冲
  check(context_query(3, buf, sizeof(buf)) < 0, "query(3) 越界被拒");
  check(context_query(-1, buf, sizeof(buf)) < 0, "query(-1) 被拒");
  check(context_query(1, buf, 16) < 0, "接收缓冲不足被拒");

  // 6. 回滚 1 个节点：只动指针，不动物理页
  check(context_rollback(1) == 0, "rollback(1)");
  r = agent_info(0, &info);
  check(r == 0 && info.count == 2, "回滚后 count == 2");
  check(context_query(2, buf, sizeof(buf)) < 0, "回滚后 query(2) 越界");
  check(context_query(1, buf, sizeof(buf)) == (int)sizeof(big),
        "回滚后最老两个节点仍可读");
  check(context_rollback(3) < 0, "rollback 超过 count 被拒");

  // 7. 配额淘汰：配额 8 KiB，灌 12 KiB 的数据
  check(context_clear() == 0, "clear");
  r = agent_info(0, &info);
  check(r == 0 && info.count == 0 && info.used == 0, "clear 后记账归零");
  for (i = 0; i < 12; i++)
    context_push(big, sizeof(big));
  r = agent_info(0, &info);
  check(r == 0 && info.evictions > 0, "超配额时自动淘汰");
  check(info.used <= (uint32)info.quota, "used 始终不超过 quota");
  check(info.count < 12 && info.count > 0, "节点数被淘汰压低");
  printf("  after flood: count=%d used=%d evictions=%d\n", (int)info.count,
         (int)info.used, (int)info.evictions);

  // 8. 单个节点超过配额：E_ARG（不是 E_QUOTA）
  check(context_push(big, sizeof(big)) == 0, "清空后可继续 push");
  check(context_query(0, buf, sizeof(buf)) == (int)sizeof(big), "淘汰后读取最老节点");

  // 9. 非法参数
  check(context_push(0, 0) < 0, "push 长度 0 被拒");
  check(context_push(big, -1) < 0, "push 负长度被拒");

  if (fails == 0)
    printf("agent_demo: PASS\n");
  else
    printf("agent_demo: FAIL (%d 项)\n", fails);

  exit(fails == 0 ? 0 : 1);
}
