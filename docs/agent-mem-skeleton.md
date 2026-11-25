# 模块 A 骨架落地说明（v1.0）

> 历史资料：本文描述 GitLab MR !1 的早期骨架。集成版采用 fork 式 Agent 创建、统一的 agent_abi.h 和可直接读写的固定槽位 Context，保留后续移植的每核页缓存与惰性批量分配。当前复现命令与接口见 [设计文档](design.md)。

> 分支：`feat/a-mem-skeleton`（基线 `dev` @ c8d82b1）
> 范围：任务一（Agent 进程 + Context 区）+ 任务三的「配额 / 淘汰」机制
> 验收口径：`make` 通过、`make qemu` 起、`agent_demo` 自检 PASS、`usertests` 无回归

## 1. 一句话设计

Context 区不挤堆也不挤栈，而是钉在**用户地址空间最高处、TRAPFRAME 正下方**的 16 页固定窗口里；
页表项只给 `R|U`，内核通过物理直映写内容——于是「用户改不了自己的上下文」是硬件保证的，不是约定。

## 2. 改动清单

| 文件 | 类型 | 作用 |
| --- | --- | --- |
| `kernel/agent_mem.c` | 新增 495 行 | 记账与节点操作、6 个系统调用实现 |
| `kernel/kalloc.c` | 改写 +219 | per-CPU 页缓存 + `kalloc_dirty/clean` + 空闲页计数 |
| `kernel/proc.h` | +24 | `enum agentloop`、`struct ctx_meta`、PCB 5 字段 |
| `kernel/memlayout.h` | +6 | `AGENT_CTX_PAGES/SIZE/BASE` |
| `kernel/proc.c` | +4 | `allocproc` 初始化 PCB、`freeproc` 归还整区 |
| `kernel/exec.c` | +18 | 新页表重新映射区；失败路径 unmap；commit 后归还旧区 |
| `kernel/defs.h` | +12 | 新函数声明 |
| `kernel/agent_proto.h` | +16 | `struct agent_info` |
| `kernel/agent_ctx.c` | 只留任务五桩 | 任务一/三迁到 `agent_mem.c` |
| `user/agent.h`、`user/agent_demo.c` | 新增 | 系统调用封装 + 40 项自检 |
| `Makefile` | +2 | `$K/agent_mem.o`、`$U/_agent_demo` |

## 3. 关键不变量

1. `AGENT_CTX_BASE = TRAPFRAME - 64 KiB`，任何用户进程都够不到（`p->sz` 远低于它），所以 `sbrk/exec` 布局零改动。
2. 节点格式 `[u32 len][u32 flags][payload 8B 对齐]`；`ctx.used == ctx.tail - ctx.head`。
3. 配额 = `min(resource_quota, 65536)`；判定唯一依据是 `ctx.used`；超了按 FIFO 淘汰最老节点（`evictions++`）。
4. 单节点 > 配额 → `AGENT_E_ARG(-2)`；区满且压缩后仍不够、或物理页耗尽 → `AGENT_E_QUOTA(-3)`，绝不 panic。
5. 内存压力时 per-CPU 缓存 + 全局链两级；缓存高于 `KMEM_HIGH` 批量回吐全局，低于 `KMEM_LOW` 批量取用。

## 4. 系统调用语义（v0）

| nr | 调用 | v0 语义 | 返回 |
| --- | --- | --- | --- |
| 23 | `agent_create(type, interval, quota)` | 把**调用者自己**升级为 Agent（创建子 Agent 留 v1） | 0 / 负错误码 |
| 24 | `agent_info(pid, *info)` | `pid<=0` 查自己，否则查进程表 | 0 / -2 / -4 |
| 27 | `context_push(buf, len)` | 追加节点，必要时压缩或淘汰 | 0 / -2 / -3 / -4 |
| 28 | `context_query(idx, buf, cap)` | 按序号读，`cap` 不足 → -2 | 实际长度 / 负错误码 |
| 29 | `context_rollback(n)` | 尾指针回退 n 个节点，不动物理页 | 0 / -2 |
| 30 | `context_clear()` | 记账归零 + 页清零复用 | 0 |

## 5. 构建与运行

```bash
cd /home/Ivancoco/dev-check
python3 /mnt/d/FS3agent/skeleton/apply.py     # 幂等地打补丁 + 落文件
make -j4                                       # 编 kernel
make fs.img                                    # 生成镜像
make qemu                                      # 进 shell
$ agent_demo                                   # 40 项自检
```

## 6. 验证结果

- `make -j4`：通过（`-Werror` 无告警），`kernel/kernel` 316928 B。
- `make fs.img`：通过，2048000 B。
- `make qemu`：`xv6 kernel is booting` → `init: starting sh` → `$`。
- `agent_demo`：**PASS**，40 项断言全绿，含 `base=0x3FFFFEE000 size=65536 free=32513`、
  淘汰场景 `count=8 used=8064 evictions=4`。
- `usertests`：**ALL TESTS PASSED**（对照：纯净 `dev` 基线 worktree 同样 PASS）。

## 7. 回归与遗留

- 回归：`usertests` 全量 PASS。过程中发现 per-CPU 缓存在"全局链已空"时会让 `countfree()` 少数 30 页（缓存对其他核不可见），已在 `kalloc_dirty` 的冷路径加 `kmem_drain_others()`：全局空时先回收其他核缓存再判定失败。
- 未做（明确留白）：
  - 任务五的 4 个调用仍是桩（heartbeat/set/stop、watch/wait/unwatch）。
  - `fork()` 后子进程**不继承 Agent 身份**：`allocproc()` 会把 5 个字段重新初始化为 0，Context 区也不在 `uvmcopy` 的 `sz` 范围内。子进程要自己 `agent_create()`。这一行为在 `agent_demo` 里没有覆盖，需要写测试固定下来。
  - 淘汰策略、重要性打分、摘要与索引（B 模块）不在本骨架内。
  - 没有性能数字（rdtime 打点留在下一版）。

## 8. 下一步建议

1. 补 `fork` 语义：子进程继承 `agent_type` 但区为空，或显式返回 `-7`。
2. 把 4 个心跳/事件桩填上，任务五闭环。
3. 加 `ctx_putv` 批量提交，压掉单次 push 的多次 `copyin`。
4. 用 `rdtime` 打点做 p50/p99，喂给赛题要求的性能对比表。
