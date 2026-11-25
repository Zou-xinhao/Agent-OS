# Agent 心跳与等待

本阶段实现任务五的定时唤醒。Agent 进程可以在内核中阻塞等待，原有工具和 Context ABI 保持兼容。

| 接口 | 行为 |
|---|---|
| `agent_create(type, interval, quota)` | 初始周期为 0 时不启动心跳，正值按 tick 计时，从子进程变为可运行时起算。 |
| `agent_heartbeat_set(interval)` | 正值设置新周期、从当前 tick 重新计时并清除旧的待处理心跳。无效参数或普通进程返回 -1。 |
| `agent_heartbeat_stop()` | 停止当前 Agent 的心跳并清除待处理心跳，可重复停止。 |
| `agent_wait(timeout)` | 大于 0 时最多等待指定 tick；0 查询一次；`AGENT_WAIT_FOREVER` 无限阻塞。收到心跳返回 `AGENT_EVENT_HEARTBEAT`，超时、无事件可取或无效调用返回 -1。 |

待处理心跳至多保留一个。时钟处理程序锁存实际触发的等待结果，同一 tick 同时发生心跳与超时时优先交付心跳。超时之后到达的心跳留给下次调用。进程阻塞时，`agent_info` 可观察到 `AGENT_LOOP_WAITING`。唤醒后恢复为 `AGENT_LOOP_IDLE`。被 `kill` 的进程由 xv6 退出路径回收。成功 `exec` 会清除 Agent 计时状态。

时钟推进与等待登记都遵循 `tickslock → p->lock`。检查待处理心跳和标记 `SLEEPING` 在同一个临界区完成。返回调度器前释放 `tickslock`，只持有 `p->lock`。hart 0 在心跳或等待期限到达时才将对应 Agent 变为可运行，普通的时钟睡眠通道保持原来的行为。

## 复现

```bash
make kernel/kernel fs.img
./test-xv6.py agenttests
./test-xv6.py -q usertests
```

`agenttests` 覆盖普通进程拒绝调用、初始心跳、心跳先到、重新设定、停止并清除待处理事件、有限超时、无限等待唤醒，以及终止一个确实处于 `SLEEPING` 状态的 Agent。通过时打印 `agenttests: heartbeat PASS`。

第二轮本地验证通过内核与镜像构建、clang-format、默认 3 hart 的 Agent 专项与 xv6 快速回归，以及 1 hart 下复跑 Agent 专项与系统调用冒烟测试。
