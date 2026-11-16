# Agent-OS：面向 AI 智能体的操作系统内核

2026 全国大学生计算机系统能力大赛 · 操作系统设计赛 · 华北区域赛 · 功能挑战赛道 · 赛题 proj61

在 xv6-riscv（rev5）之上扩展出 Agent 感知的内核能力：Agent 进程抽象、结构化工具调用协议、
上下文路径管理、Agent Loop 运行机制，并以「Agent 系统管理员」场景做综合验证。

> 一句话：让内核不仅能跑进程，还能"知道有个 Agent 在用工具、并且记得它刚才用过什么"。

---

## 团队

| 角色 | 姓名 | 负责 | 主要文件 |
|---|---|---|---|
| A 内核进程主线（组长） | | 任务一 / 任务三 / 任务五 | `kernel/proc.c` `kernel/vm.c` `kernel/trap.c` `kernel/sysproc.c` `kernel/agent_ctx.c` |
| B 内核接口与文件系统 | | 任务二 / 任务四（加分） | `kernel/syscall.c` `kernel/sysfile.c` `kernel/fs.c` `kernel/file.c` `kernel/agent_proto.h` `kernel/agent_tool.c` |
| C 验证与交付 | | 测试 / 用户态 Agent / 任务六场景B / 文档视频 | `user/` `scripts/` `docs/` `README.md` |

---

## 环境（三人必须一致）

| 组件 | 版本 | 说明 |
|---|---|---|
| 宿主 | Windows 11 + WSL2 | 代码放 `~/agent-os`，不要放 `/mnt/d` |
| 发行版 | Ubuntu 24.04 | |
| gcc | riscv64-linux-gnu-gcc 13.3.0 | |
| QEMU | qemu-system-riscv64 8.2.x | 包名 `qemu-system-misc` |
| GDB | gdb-multiarch 15.x | |

```bash
sudo apt update && sudo apt install -y build-essential git gdb-multiarch \
    qemu-system-misc gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu bear
bash scripts/check-env.sh          # 版本自检，三人输出必须一致
```

环境不一致的兜底：`docker build -t agent-os . && docker run -it -v $PWD:/work agent-os`
（VS Code 里也可直接用 `.devcontainer/` 的 Reopen in Container）。

---

## 快速开始

```bash
git clone https://github.com/<你们>/agent-os.git && cd agent-os
make qemu          # 看到 "xv6 kernel is booting" 和 $ 提示符即成功
```

调试：

```bash
make qemu-gdb        # 另开一个终端
gdb-multiarch -x .gdbinit
(gdb) b sys_tool_call
(gdb) c
```

运行测试（C 负责维护）：

```bash
make test
```

---

## 目录结构

```
kernel/       内核：proc/vm/trap/syscall/fs + 新增 agent_ctx.c agent_tool.c
user/         用户态：shell、Agent 示例程序、测试程序
docs/         设计文档、协作规范、周报
scripts/      构建/测试/环境检查脚本
demo/         每周 30 秒演示视频（解说视频素材）
mkfs/         文件系统镜像工具
Makefile
```

---

## 任务与模块对应

| 赛题任务 | 权重 | 实现位置 | 负责人 |
|---|---|---|---|
| 任务一 Agent 进程与地址空间 | 基础 40% | `proc.h/ proc.c/ sysproc.c/ vm.c` | A |
| 任务二 结构化交互接口 | 基础 40% | `agent_proto.h/ agent_tool.c/ syscall.c` | B |
| 任务三 上下文路径管理 | 基础 40% | `agent_ctx.c/ proc.c` | A |
| 任务四 文件系统扩展（属性） | 进阶 35% | `fs.c/ sysfile.c`（加分项） | B |
| 任务五 Agent Loop 机制 | 进阶 35% | `trap.c/ proc.c`（心跳 + 事件） | A |
| 任务六 场景B Agent 系统管理员 | 综合 25% | `user/agentsysadmin.c` | C |

系统调用编号 22–37 见 `docs/接口约定.md`，**改动需三人确认**。

---

## 参考与借鉴声明

本项目基于 MIT PDOS 的 xv6-riscv（rev5，commit `<填入 hash>`），其许可证与版权声明保留在
`LICENSE` 与源码头注释中。在此基础上新增的 Agent 相关模块、系统调用与文档均为本队原创；
如有引用第三方代码或思路，会在对应文件头部与本表登记。

| 来源 | 用途 | 增量 |
|---|---|---|
| xv6-riscv rev5 (MIT) | 教学内核基底 | 全部 Agent 扩展为新写 |
| | | |
