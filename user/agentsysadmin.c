// 单次手动巡检。工作进程和巡检 Agent 都是本演示自己的子进程。
#include "kernel/param.h"
#include "user/agent.h"
#include "user/user.h"

#define WORKER_WAIT_TICKS 10

struct tool_arg {
  const char *key;
  const char *value;
};

static struct {
  struct agent_resp header;
  char data[NPROC * sizeof(struct agent_proc_row)];
} reply;

static int
fail(const char *step)
{
  fprintf(2, "agentsysadmin: FAIL %s\n", step);
  return 1;
}

// 请求头后先排列所有长度头，再放入各键和值的字节。
static int
call_tool(uint16 id, const struct tool_arg *args, uint16 argc)
{
  struct {
    struct agent_req header;
    char payload[64];
  } request = {.header = {.magic = AGENT_MAGIC, .tool_id = id, .argc = argc}};
  uint offset = argc * sizeof(struct agent_kv);

  if (offset > sizeof(request.payload))
    return -1;
  for (uint i = 0; i < argc; i++) {
    uint key_len = strlen(args[i].key);
    uint value_len = strlen(args[i].value);
    if (key_len + value_len > sizeof(request.payload) - offset)
      return -1;
    struct agent_kv len = {.klen = key_len, .vlen = value_len};
    memmove(request.payload + i * sizeof(len), &len, sizeof(len));
    memmove(request.payload + offset, args[i].key, key_len);
    offset += key_len;
    memmove(request.payload + offset, args[i].value, value_len);
    offset += value_len;
  }
  request.header.payload_len = offset;
  if (tool_call(&request, sizeof(request.header) + offset, &reply,
                sizeof(reply)) != AGENT_OK ||
      reply.header.magic != AGENT_MAGIC || reply.header.reserved != 0 ||
      reply.header.status != AGENT_OK ||
      reply.header.data_len > sizeof(reply.data))
    return -1;
  return reply.header.data_len;
}

static int
observe_worker(int worker)
{
  const struct tool_arg args[] = {
    {"type", "ordinary"},
    {"status", "sleeping"},
  };
  // Worker 可能尚未执行到管道 read。等待一个 tick 后重新采样，
  // 并限制总尝试次数，避免因调度顺序产生不稳定的判断。
  for (int tick = 0; tick <= WORKER_WAIT_TICKS; tick++) {
    int len = call_tool(TOOL_QUERY_PROCESS, args, 2);
    if (len < 0 || len % sizeof(struct agent_proc_row) != 0)
      return -1;
    for (int offset = 0; offset < len;
         offset += sizeof(struct agent_proc_row)) {
      struct agent_proc_row row;
      memmove(&row, reply.data + offset, sizeof(row));
      if (row.pid == worker)
        return row.type == AGENT_TYPE_NORMAL && row.state == AGENT_PROC_SLEEPING
                 ? 0
                 : -1;
    }
    if (tick == WORKER_WAIT_TICKS || pause(1) < 0)
      break;
  }
  return -1;
}

static int
inspect_once(int worker)
{
  struct agent_system_status status;
  char status_note[] = "get_system_status result=system-running";
  char decision_note[] = "query_process result=ordinary/sleeping action=keep";
  char history_note[] = "read_context index=1 result=decision-confirmed";
  const struct tool_arg history_arg = {"index", "1"};
  struct agent_record history;

  if (call_tool(TOOL_GET_SYSTEM_STATUS, 0, 0) != sizeof(status))
    return fail("system status reply");
  memmove(&status, reply.data, sizeof(status));
  if (status.processes[AGENT_PROC_RUNNING] == 0 ||
      context_push(status_note, sizeof(status_note) - 1) != AGENT_OK)
    return fail("system status or Context append");
  printf("agentsysadmin: status running=%d sleeping=%d\n",
         (int)status.processes[AGENT_PROC_RUNNING],
         (int)status.processes[AGENT_PROC_SLEEPING]);

  if (observe_worker(worker) < 0)
    return fail("expected sleeping worker");
  // 管道上的休眠是预期的，本轮只记录决定并让 worker 保持原状。
  if (context_push(decision_note, sizeof(decision_note) - 1) != AGENT_OK)
    return fail("decision Context append");
  printf("agentsysadmin: worker=ordinary/sleeping action=keep\n");

  if (call_tool(TOOL_READ_CONTEXT, &history_arg, 1) != sizeof(history))
    return fail("history reply");
  memmove(&history, reply.data, sizeof(history));
  if (history.len != sizeof(decision_note) - 1 ||
      memcmp(history.data, decision_note, history.len) != 0 ||
      context_push(history_note, sizeof(history_note) - 1) != AGENT_OK)
    return fail("history verification");
  printf("agentsysadmin: history index=1 matched\n");
  return 0;
}

int
main(void)
{
  int gate[2];
  if (pipe(gate) < 0)
    exit(fail("worker pipe"));
  int worker = fork();
  if (worker < 0) {
    close(gate[0]);
    close(gate[1]);
    exit(fail("worker fork"));
  }
  if (worker == 0) {
    char ignored;
    close(gate[1]);
    int result = read(gate[0], &ignored, 1);
    close(gate[0]);
    exit(result == 0 ? 0 : 1);
  }
  close(gate[0]);

  int admin = agent_create(AGENT_TYPE_AGENT, 0, 4096);
  if (admin == 0) {
    close(gate[1]);
    exit(inspect_once(worker));
  }

  // 父进程保留唯一的写端直到巡检结束。关闭它后 worker 正常退出，
  // 创建 Agent 失败时也走同一条清理路径。
  int admin_status = -1;
  int admin_ok = admin > 0 && wait(&admin_status) == admin && admin_status == 0;
  close(gate[1]);
  int worker_status = -1;
  int worker_ok = wait(&worker_status) == worker && worker_status == 0;
  if (!admin_ok || !worker_ok)
    exit(fail("child exit status"));
  printf("agentsysadmin: PASS\n");
  exit(0);
}
