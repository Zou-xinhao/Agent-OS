// 物理内存布局

// qemu -machine virt 的布局就是这样，
// 依据 qemu 的 hw/riscv/virt.c：
//
// 00001000 -- 启动 ROM，由 qemu 提供
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio 磁盘
// 80000000 -- qemu 的启动 ROM 把内核装载到这里，
//             然后跳到这里执行。
// 80000000 之后的 RAM 未被使用。

// 内核这样使用物理内存：
// 80000000 -- entry.S，然后是内核代码与数据
// end -- 内核页分配区的起始位置
// PHYSTOP -- 内核所用 RAM 的结束位置

// qemu 把 UART 寄存器放在这里的物理内存中。
#define UART0     0x10000000L
#define UART0_IRQ 10

// virtio mmio 接口
#define VIRTIO0     0x10001000
#define VIRTIO0_IRQ 1

// 核局部中断控制器(CLINT)
#define CLINT_BASE  0x02000000L
#define CLINT(hart) (CLINT_BASE + (hart) * 4)

// qemu 把平台级中断控制器(PLIC)放在这里。
#define PLIC                 0x0c000000L
#define PLIC_PRIORITY        (PLIC + 0x0)
#define PLIC_PENDING         (PLIC + 0x1000)
#define PLIC_SENABLE(hart)   (PLIC + 0x2080 + (hart) * 0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart)    (PLIC + 0x201004 + (hart) * 0x2000)

// 内核假定物理地址 0x80000000 到 PHYSTOP
// 之间是可供内核与用户页
// 使用的 RAM。
#define KERNBASE 0x80000000L
#define PHYSTOP  (KERNBASE + 128 * 1024 * 1024)

// 把跳板页(trampoline)映射到最高地址，
// 用户空间与内核空间都是如此。
#define TRAMPOLINE (MAXVA - PGSIZE)

// 把内核栈映射到跳板页下方，
// 每个栈两侧各有一个无效的保护页(guard page)。
#define KSTACK(p) (TRAMPOLINE - ((p) + 1) * 2 * PGSIZE)

// 用户内存布局。
// 从地址 0 开始依次是：
//   text
//   原始 data 与 bss
//   固定大小的栈
//   可扩展的堆
//   ...
//   TRAPFRAME(p->trapframe，供跳板页使用)
//   TRAMPOLINE(与内核中的那一页相同)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)

// 仅供 Agent 使用的高位固定映射：位于 TRAPFRAME 之下、中间隔着 AGENT_MAX_QUOTA，其下方再留一页不映射。
#define AGENT_CONTEXT_BASE (TRAPFRAME - AGENT_MAX_QUOTA)
#define AGENT_HEAP_LIMIT   (AGENT_CONTEXT_BASE - PGSIZE)
