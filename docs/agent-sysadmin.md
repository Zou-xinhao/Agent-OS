# 手动巡检演示

`agentsysadmin` 在 xv6 中运行一次巡检。普通父进程启动一个在管道上等待的普通 worker，再用 `agent_create` 创建巡检 Agent。Agent 调用 `get_system_status` 读取进程统计，用 `query_process` 按进程类型和睡眠状态找回这个已知 worker。对预期中的休眠，演示策略选择保留进程并记录决定。Agent 把请求和结果摘要写入自己的 Context，并通过 `read_context` 读回、核对历史记录。巡检结束后，父进程关闭管道并回收两个子进程。

演示直接使用基础版的数字工具 ID，可以独立于心跳和事件唤醒运行。它只观察自己创建的 worker 并记录决定。下列命令在 Ubuntu 的项目目录运行。

```bash
make kernel/kernel fs.img
./test-xv6.py agentsysadmin
```

也可执行 `make qemu`，在 xv6 的 `$` 提示符输入 `agentsysadmin`。进程计数随当时的调度状态变化。通过时，日志依次显示系统状态、`worker=ordinary/sleeping action=keep`、`history index=1 matched` 和 `agentsysadmin: PASS`。自动测试核对这些输出，子进程失败、历史不一致或超时都会使测试失败。

本轮在未加入心跳功能的比赛基线 `c6eddec` 上验证了独立构建、巡检三段输出、子进程正常回收和原有六轮 Agent 回归，均通过。
