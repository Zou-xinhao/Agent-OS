//
// Agent-OS 工具调用协议（三人共享，B 维护，A/C 只读）
// 与《执行计划》第四部分「三人接口约定」逐字段一致，改动必须三人同意。
//
#ifndef AGENT_PROTO_H
#define AGENT_PROTO_H

#define AGENT_MAGIC       0xA6E7
#define AGENT_MAX_PAYLOAD 4096

// 错误码：0 成功，负数为失败原因
#define AGENT_OK      0
#define AGENT_E_TOOL  (-1) // 工具不存在
#define AGENT_E_ARG   (-2) // 参数缺失或格式错
#define AGENT_E_QUOTA (-3) // 上下文区配额不足
#define AGENT_E_FAULT (-4) // 用户指针非法
#define AGENT_E_BUSY  (-5) // 资源被占用
#define AGENT_E_IO    (-6) // 文件系统 I/O 失败
#define AGENT_E_NOSYS (-7) // 功能未实现

// 工具编号（sys_tool_call 的 tool_id）
#define TOOL_QUERY_PROCESS     1
#define TOOL_QUERY_FILE        2
#define TOOL_READ_CONTEXT      3
#define TOOL_SEND_MESSAGE      4
#define TOOL_GET_SYSTEM_STATUS 5

// 请求头：紧随其后是 argc 条 agent_kv（key 在前、value 在后，长度由 klen/vlen 给出）
struct agent_req {
  uint16 magic;       // 必须等于 AGENT_MAGIC
  uint16 tool_id;     // 见上面的 TOOL_*
  uint16 argc;        // 参数个数
  uint16 flags;       // 保留，必须为 0
  uint32 payload_len; // 参数区总字节数（<= AGENT_MAX_PAYLOAD）
};

// 单条参数的描述头，实际字节紧跟在所有 kv 头之后
struct agent_kv {
  uint16 klen;
  uint16 vlen;
};

// 应答头：data_len 字节的 data 紧随其后
struct agent_resp {
  uint16 magic;    // AGENT_MAGIC
  uint16 reserved; // 0
  int status;      // AGENT_OK 或负错误码
  uint32 data_len;
};

#endif // AGENT_PROTO_H
