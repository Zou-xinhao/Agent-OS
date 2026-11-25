#include "kernel/types.h"
#include "kernel/agent_abi.h"
#include "kernel/agent_proto.h"
#include "user/user.h"

#define TEST_PAGE 4096

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(2, "agenttests: FAIL line %d: %s\n", __LINE__, #condition);      \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

struct reply {
  struct agent_resp header;
  char data[2048];
};

static struct reply reply;

static int
call(int tool, const char *key1, const char *value1, const char *key2,
     const char *value2, int capacity)
{
  struct agent_req req = {.magic = AGENT_MAGIC, .tool_id = tool};
  struct agent_kv kv[2];
  const char *keys[] = {key1, key2};
  const char *values[] = {value1, value2};
  char packet[sizeof(req) + sizeof(kv) + 80];

  req.argc = key2 ? 2 : key1 != 0;
  uint offset = sizeof(req) + req.argc * sizeof(kv[0]);
  for (int i = 0; i < req.argc; i++) {
    kv[i].klen = strlen(keys[i]);
    kv[i].vlen = strlen(values[i]);
    CHECK(offset + kv[i].klen + kv[i].vlen <= sizeof(packet));
    memmove(packet + offset, keys[i], kv[i].klen);
    offset += kv[i].klen;
    memmove(packet + offset, values[i], kv[i].vlen);
    offset += kv[i].vlen;
  }
  req.payload_len = offset - sizeof(req);
  memmove(packet, &req, sizeof(req));
  memmove(packet + sizeof(req), kv, req.argc * sizeof(kv[0]));

  int status = tool_call(packet, offset, &reply, capacity);
  CHECK(reply.header.magic == AGENT_MAGIC && reply.header.reserved == 0);
  CHECK(reply.header.status == status &&
        reply.header.data_len <= sizeof(reply.data));
  if (status < 0)
    CHECK(reply.header.data_len == 0);
  return status;
}

static struct agent_info
self_info(void)
{
  struct agent_info info;
  int pid = getpid();
  CHECK(agent_info(pid, &info) == 0);
  CHECK(info.pid == pid && info.type == AGENT_TYPE_AGENT);
  CHECK(info.context_base != 0);
  return info;
}

static void
wait_ok(int pid)
{
  int status = -1;
  CHECK(wait(&status) == pid && status == 0);
}

static void
check_tools_and_loop(void)
{
  char list[180];
  int len = tool_list(0, 0);
  CHECK(len > 0 && len < sizeof(list));
  CHECK(tool_list(list, len - 1) == AGENT_E_QUOTA);
  CHECK(tool_list(list, len) == len);
  CHECK(memcmp(list, "1 query_process", strlen("1 query_process")) == 0);

  for (int round = 0; round < 6; round++) {
    const char *summary;
    if (round % 3 == 0) {
      CHECK(call(TOOL_QUERY_PROCESS, "status", "running", "type", "agent",
                 sizeof(reply)) == AGENT_OK);
      CHECK(reply.header.data_len % sizeof(struct agent_proc_row) == 0);
      int found = 0;
      for (uint i = 0; i < reply.header.data_len;
           i += sizeof(struct agent_proc_row)) {
        struct agent_proc_row row;
        memmove(&row, reply.data + i, sizeof(row));
        CHECK(row.state == AGENT_PROC_RUNNING && row.type == AGENT_TYPE_AGENT);
        found |= row.pid == getpid();
      }
      CHECK(found);
      summary = "query_process: found current Agent";
    } else if (round % 3 == 1) {
      CHECK(call(TOOL_GET_SYSTEM_STATUS, 0, 0, 0, 0, sizeof(reply)) ==
            AGENT_OK);
      CHECK(reply.header.data_len == sizeof(struct agent_system_status));
      struct agent_system_status status;
      memmove(&status, reply.data, sizeof(status));
      CHECK(status.processes[AGENT_PROC_RUNNING] > 0);
      CHECK(status.uptime_ticks <= (uint)uptime());
      summary = "get_system_status: scheduler active";
    } else {
      CHECK(call(TOOL_READ_CONTEXT, "index", "0", 0, 0, sizeof(reply)) ==
            AGENT_OK);
      CHECK(reply.header.data_len == sizeof(struct agent_record));
      struct agent_record record;
      memmove(&record, reply.data, sizeof(record));
      const char *first = "query_process: found current Agent";
      CHECK(record.len == strlen(first));
      CHECK(memcmp(record.data, first, record.len) == 0);
      summary = "read_context: verified first result";
    }

    CHECK(context_push((void *)summary, strlen(summary)) == 0);
    struct agent_info info = self_info();
    CHECK(info.count == round + 1);
    struct agent_record *records = (struct agent_record *)info.context_base;
    struct agent_record *record =
      &records[(info.head + round) % (info.quota / sizeof(*record))];
    char buf[AGENT_CONTEXT_DATA];
    CHECK(context_query(round, buf, sizeof(buf)) == strlen(summary));
    CHECK(record->len == strlen(summary));
    CHECK(memcmp(record->data, buf, record->len) == 0);
    printf("agenttests: round %d: %s\n", round + 1, summary);
  }
}

static void
check_errors(void)
{
  struct agent_req req = {.magic = AGENT_MAGIC,
                          .tool_id = TOOL_GET_SYSTEM_STATUS};
  CHECK(call(999, 0, 0, 0, 0, sizeof(reply)) == AGENT_E_TOOL);
  CHECK(call(TOOL_QUERY_PROCESS, "status", "bogus", 0, 0, sizeof(reply)) ==
        AGENT_E_ARG);
  CHECK(call(TOOL_QUERY_PROCESS, "type", "agent", "type", "ordinary",
             sizeof(reply)) == AGENT_E_ARG);
  CHECK(call(TOOL_READ_CONTEXT, "index", "2147483648", 0, 0, sizeof(reply)) ==
        AGENT_E_ARG);
  CHECK(call(TOOL_READ_CONTEXT, 0, 0, 0, 0, sizeof(reply)) == AGENT_E_ARG);
  CHECK(call(TOOL_GET_SYSTEM_STATUS, 0, 0, 0, 0, sizeof(struct agent_resp)) ==
        AGENT_E_QUOTA);

  CHECK(tool_call(&req, -1, &reply, sizeof(reply)) == AGENT_E_ARG);
  CHECK(reply.header.status == AGENT_E_ARG && reply.header.data_len == 0);
  req.flags = 1;
  CHECK(tool_call(&req, sizeof(req), &reply, sizeof(reply)) == AGENT_E_ARG);
  req.flags = 0;
  req.payload_len = 1;
  CHECK(tool_call(&req, sizeof(req), &reply, sizeof(reply)) == AGENT_E_ARG);
  struct {
    struct agent_req header;
    struct agent_kv kv;
  } bad = {{.magic = AGENT_MAGIC,
            .tool_id = TOOL_READ_CONTEXT,
            .argc = 1,
            .payload_len = sizeof(struct agent_kv)},
           {.klen = 1, .vlen = 1}};
  CHECK(tool_call(&bad, sizeof(bad), &reply, sizeof(reply)) == AGENT_E_ARG);
  req.payload_len = 0;
  CHECK(tool_call(&req, sizeof(req), (void *)-1, sizeof(reply)) ==
        AGENT_E_FAULT);
  CHECK(tool_call((void *)-1, sizeof(req), &reply, sizeof(reply)) ==
        AGENT_E_FAULT);
  CHECK(context_push((void *)-1, 1) == AGENT_E_FAULT);
  CHECK(context_push(&req, AGENT_CONTEXT_DATA + 1) == AGENT_E_QUOTA);
  CHECK(context_query(0, &req, 0) == AGENT_E_QUOTA);
  CHECK(context_query(-1, &req, sizeof(req)) == AGENT_E_ARG);
  CHECK(context_rollback(-1) == AGENT_E_ARG &&
        context_rollback(7) == AGENT_E_ARG);
  CHECK(self_info().count == 6);
}

static void
check_fifo_and_isolation(void)
{
  CHECK(context_clear() == 0);
  struct agent_info info = self_info();
  int slots = info.quota / sizeof(struct agent_record);

  for (int i = 0; i < slots * 3 + 5; i++)
    CHECK(context_push(&i, sizeof(i)) == 0);
  info = self_info();
  CHECK(info.count == slots && info.head == 5);
  for (int i = 0; i < slots; i++) {
    int value = -1;
    CHECK(context_query(i, &value, sizeof(value)) == sizeof(value));
    CHECK(value == slots * 2 + 5 + i);
  }
  CHECK(context_rollback(2) == 0 && self_info().count == slots - 2);
  int marker = 99;
  CHECK(context_push(&marker, sizeof(marker)) == 0);

  struct agent_record *records = (struct agent_record *)info.context_base;
  records[info.head].len = AGENT_CONTEXT_DATA + 1;
  int oldest = -1;
  CHECK(context_query(0, &oldest, sizeof(oldest)) == sizeof(oldest));
  CHECK(oldest == slots * 2 + 5);
  int pid = agent_create(AGENT_TYPE_AGENT, 0, TEST_PAGE);
  CHECK(pid >= 0);
  if (pid == 0) {
    struct agent_info child = self_info();
    CHECK(child.count == 0 && child.context_base == info.context_base);
    CHECK(agent_info(info.pid, &child) == -1);
    int value = 1234;
    CHECK(context_push(&value, sizeof(value)) == 0);
    exit(0);
  }
  wait_ok(pid);
  CHECK(records[info.head].len == AGENT_CONTEXT_DATA + 1);
  CHECK(self_info().count == slots - 1);
  int value = 0;
  CHECK(context_query(slots - 2, &value, sizeof(value)) == sizeof(value));
  CHECK(value == marker);
  CHECK(context_clear() == 0 && self_info().count == 0);
  CHECK(context_query(0, &value, sizeof(value)) == AGENT_E_ARG);
}

static void
check_heartbeat(void)
{
  int status;
  uint started;

  CHECK(agent_wait(-2) == -1);
  CHECK(agent_heartbeat_set(0) == -1 && agent_heartbeat_set(-1) == -1);
  CHECK(agent_wait(20) == AGENT_EVENT_HEARTBEAT); // agent_create 的初始周期

  CHECK(agent_heartbeat_set(2) == 0);
  CHECK(pause(3) == 0);
  CHECK(agent_wait(0) == AGENT_EVENT_HEARTBEAT); // 心跳先于等待到达
  CHECK(pause(3) == 0);
  CHECK(agent_heartbeat_set(40) == 0);
  CHECK(self_info().heartbeat_interval == 40);
  CHECK(agent_wait(0) == -1); // 调频清除旧周期的待处理事件
  CHECK(agent_wait(2) == -1);
  CHECK(agent_heartbeat_set(2) == 0);
  CHECK(agent_wait(8) == AGENT_EVENT_HEARTBEAT); // 调整为更短周期
  CHECK(agent_wait(AGENT_WAIT_FOREVER) == AGENT_EVENT_HEARTBEAT);
  CHECK(agent_heartbeat_set(3) == 0);
  CHECK(agent_wait(3) == AGENT_EVENT_HEARTBEAT); // 同 tick 心跳优先
  CHECK(self_info().loop_state == AGENT_LOOP_IDLE);

  CHECK(pause(3) == 0);
  CHECK(agent_heartbeat_stop() == 0 && agent_heartbeat_stop() == 0);
  CHECK(self_info().heartbeat_interval == 0);
  CHECK(agent_wait(0) == -1);
  started = uptime();
  CHECK(agent_wait(3) == -1);
  CHECK((uint)uptime() - started >= 3);
  CHECK(self_info().loop_state == AGENT_LOOP_IDLE);

  int pid = agent_create(AGENT_TYPE_AGENT, 0, TEST_PAGE);
  CHECK(pid >= 0);
  if (pid == 0) {
    agent_wait(AGENT_WAIT_FOREVER);
    exit(42); // 未启用心跳，正常情况下不会返回用户态
  }

  struct agent_info child;
  int waiting = 0;
  for (int i = 0; i < 20; i++) {
    CHECK(agent_info(pid, &child) == 0);
    if (child.loop_state == AGENT_LOOP_WAITING) {
      waiting = 1;
      break;
    }
    CHECK(pause(1) == 0);
  }
  CHECK(waiting);

  CHECK(call(TOOL_QUERY_PROCESS, "status", "sleeping", "type", "agent",
             sizeof(reply)) == AGENT_OK);
  int found = 0;
  for (uint i = 0; i < reply.header.data_len;
       i += sizeof(struct agent_proc_row)) {
    struct agent_proc_row row;
    memmove(&row, reply.data + i, sizeof(row));
    found |= row.pid == pid && row.state == AGENT_PROC_SLEEPING;
  }
  CHECK(found);
  CHECK(kill(pid) == 0);
  CHECK(wait(&status) == pid && status == -1);
  printf("agenttests: heartbeat PASS (set, stop, sleep, timeout, kill)\n");
}

static void
check_lifecycle(void)
{
  char *heap = sbrk(0);
  CHECK(sbrk(TEST_PAGE) == heap);
  heap[0] = 42;
  CHECK(sbrk(-TEST_PAGE) == heap + TEST_PAGE);
  CHECK(sbrklazy(TEST_PAGE) == heap);
  heap[0] = 43;
  CHECK(heap[0] == 43 && sbrk(-TEST_PAGE) == heap + TEST_PAGE);

  int child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    struct agent_info info;
    CHECK(agent_info(getpid(), &info) == -1);
    CHECK(context_clear() == AGENT_E_ARG);
    exit(0);
  }
  wait_ok(child);

  int marker = 25;
  CHECK(context_push(&marker, sizeof(marker)) == 0);
  char *missing[] = {"no-agent-binary", 0};
  CHECK(exec(missing[0], missing) == -1 && self_info().count == 1);
  char *probe[] = {"agenttests", "--exec-probe", 0};
  exec(probe[0], probe);
  CHECK(0);
}

int
main(int argc, char **argv)
{
  struct agent_info info;
  if (argc == 2 && strcmp(argv[1], "--exec-probe") == 0) {
    CHECK(agent_info(getpid(), &info) == -1 && context_clear() == AGENT_E_ARG);
    exit(0);
  }

  CHECK(agent_info(getpid(), &info) == -1);
  CHECK(agent_heartbeat_set(1) == -1 && agent_heartbeat_stop() == -1);
  CHECK(agent_wait(0) == -1);
  CHECK(agent_create(0, 0, TEST_PAGE) == -1);
  CHECK(agent_create(AGENT_TYPE_AGENT, -1, TEST_PAGE) == -1);
  CHECK(agent_create(AGENT_TYPE_AGENT, 0, TEST_PAGE - 1) == -1);
  CHECK(agent_create(AGENT_TYPE_AGENT, 0, AGENT_MAX_QUOTA + TEST_PAGE) == -1);
  int gate[2];
  CHECK(pipe(gate) == 0);
  int pid = agent_create(AGENT_TYPE_AGENT, 10, TEST_PAGE);
  CHECK(pid >= 0);
  if (pid == 0) {
    char ready;
    close(gate[1]);
    CHECK(read(gate[0], &ready, 1) == 1);
    close(gate[0]);
    info = self_info();
    CHECK(info.heartbeat_interval == 10 && info.loop_state == AGENT_LOOP_IDLE);
    CHECK(info.quota == TEST_PAGE && info.count == 0 && info.head == 0);
    check_tools_and_loop();
    check_errors();
    check_fifo_and_isolation();
    check_heartbeat();
    check_lifecycle();
    exit(1);
  }
  close(gate[0]);
  CHECK(agent_info(pid, &info) == 0);
  CHECK(info.pid == pid && info.context_base == 0 && info.quota == TEST_PAGE);
  CHECK(write(gate[1], "g", 1) == 1);
  close(gate[1]);
  wait_ok(pid);
  CHECK(agent_info(pid, &info) == -1);

  // 每个 create/exit 循环都必须完整回收其上下文(Context)预留。
  for (int i = 0; i < 2048; i++) {
    pid = agent_create(AGENT_TYPE_AGENT, 0, AGENT_MAX_QUOTA);
    CHECK(pid >= 0);
    if (pid == 0) {
      struct agent_info agent = self_info();
      volatile char *context = (volatile char *)agent.context_base;
      for (uint offset = 0; offset < agent.quota; offset += TEST_PAGE) {
        CHECK(context[offset] == 0);
        context[offset] = 42;
        CHECK(context[offset] == 42);
      }
      exit(0);
    }
    wait_ok(pid);
  }
  printf("agenttests: PASS\n");
  exit(0);
}
