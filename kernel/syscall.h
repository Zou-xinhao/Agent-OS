// 系统调用号
#define SYS_fork   1
#define SYS_exit   2
#define SYS_wait   3
#define SYS_pipe   4
#define SYS_read   5
#define SYS_kill   6
#define SYS_exec   7
#define SYS_fstat  8
#define SYS_chdir  9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_pause  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
#define SYS_mknod  17
#define SYS_unlink 18
#define SYS_link   19
#define SYS_mkdir  20
#define SYS_close  21
#define SYS_sync   22
// ---- Agent-OS 扩展（23-38），编号一经确定不再改动 ----
// 任务一：Agent 进程与地址空间
#define SYS_agent_create 23
#define SYS_agent_info   24
// 任务二：结构化交互接口
#define SYS_tool_call 25
#define SYS_tool_list 26
// 任务三：上下文路径管理
#define SYS_context_push     27
#define SYS_context_query    28
#define SYS_context_rollback 29
#define SYS_context_clear    30
// 任务五：Agent Loop 机制
#define SYS_agent_heartbeat_set  31
#define SYS_agent_heartbeat_stop 32
#define SYS_agent_watch          33
#define SYS_agent_wait           34
#define SYS_agent_unwatch        35
// 任务四：文件系统属性扩展（加分项）
#define SYS_file_setattr 36
#define SYS_file_getattr 37
#define SYS_file_query   38
