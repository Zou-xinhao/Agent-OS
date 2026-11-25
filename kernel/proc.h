#include "agent_abi.h"

// 内核上下文切换保存的寄存器。
struct context {
  uint64 ra;
  uint64 sp;

  // 被调用者保存
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// 每核状态。
struct cpu {
  struct proc *proc;      // 本 CPU 上运行的进程，没有则为 null。
  struct context context; // 在此处 swtch() 进入调度器(scheduler)。
  int noff;               // push_off() 的嵌套深度。
  int intena;             // push_off() 之前中断是否开启？
};

extern struct cpu cpus[NCPU];

// 供 trampoline.S 中陷入处理代码使用的每进程数据。
// 它独占一页，位于用户页表中跳板页(trampoline)的正下方。
// 在内核页表中不做专门映射。
// trampoline.S 中的 uservec 把用户寄存器保存到陷入帧(trapframe)，
// 然后用陷入帧里的
// kernel_sp、kernel_hartid、kernel_satp 初始化寄存器，并跳转到 kernel_trap。
// trampoline.S 中的 prepare_return() 与 userret 负责设置
// 陷入帧的 kernel_* 字段，从陷入帧恢复用户寄存器，
// 切换到用户页表，并进入用户空间。
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // 内核页表(page table)
  /*   8 */ uint64 kernel_sp;     // 进程内核栈的栈顶
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // 保存的用户程序计数器
  /*  32 */ uint64 kernel_hartid; // 保存的内核 tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// 独立于用户态 ABI。所有字段由所属进程的 p->lock 保护。
struct agent_timing {
  uint heartbeat_tick; // 最近一次心跳的计划时刻
  uint wait_tick;      // 本次等待开始的时刻
  int wait_timeout;
  int wait_result;
  int heartbeat_pending;
};

// 每进程状态
struct proc {
  struct spinlock lock;

  // 使用以下字段时必须持有 p->lock：
  enum procstate state;    // 进程状态
  void *chan;              // 非 0 表示在 chan 上睡眠
  int killed;              // 非 0 表示已被杀死
  int xstate;              // 返回给父进程 wait 的退出状态
  int pid;                 // 进程 ID
  struct agent_info agent; // 内核持有的 Agent 元数据
  uint16 agent_lengths[AGENT_CONTEXT_SLOTS];
  struct agent_timing agent_timing;

  // 使用该字段时必须持有 wait_lock：
  struct proc *parent; // 父进程

  // 以下字段为进程私有，无需持有 p->lock。
  uint64 kstack;               // 内核栈的虚拟地址
  uint64 sz;                   // 进程内存大小（字节）
  pagetable_t pagetable;       // 用户页表
  struct trapframe *trapframe; // 供 trampoline.S 使用的数据页
  struct context context;      // 在此处 swtch() 运行该进程
  struct file *ofile[NOFILE];  // 打开的文件
  struct inode *cwd;           // 当前目录
  char name[16];               // 进程名（调试用）
};
