#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// 位于 kernelvec.S，调用 kerneltrap()。
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// 设置在内核中接收异常和陷入的环境。
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// 处理来自用户空间的中断、异常或系统调用。
// 由 trampoline.S 调用，并返回到 trampoline.S。
// 返回值是供 trampoline.S 切换的用户 satp。
//
uint64
usertrap(void)
{
  int which_dev = 0;

  if ((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // 把中断和异常交给 kerneltrap()，
  // 因为现在已经在内核中。
  w_stvec((uint64)kernelvec); //DOC: kernelvec

  struct proc *p = myproc();

  // 保存用户程序计数器。
  p->trapframe->epc = r_sepc();

  if (r_scause() == 8) {
    // 系统调用

    if (killed(p))
      kexit(-1);

    // sepc 指向 ecall 指令，
    // 但要返回到下一条指令。
    p->trapframe->epc += 4;

    // 中断会改变 sepc、scause 和 sstatus，
    // 所以在处理完这些寄存器之后才开中断。
    intr_on();

    syscall();
  } else if ((which_dev = devintr()) != 0) {
    // 正常
  } else if ((r_scause() == 15 || r_scause() == 13) &&
             vmfault(p->pagetable, p->sz, r_stval(),
                     (r_scause() == 13) ? 1 : 0) != 0) {
    // 惰性分配页上的缺页
  } else {
    printk("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printk("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if (killed(p))
    kexit(-1);

  // 如果是定时器中断，就让出 CPU。
  if (which_dev == 2)
    yield();

  prepare_return();

  // 供 trampoline.S 切换用的用户页表
  uint64 satp = MAKE_SATP(p->pagetable);

  // 返回 trampoline.S；satp 的值放在 a0 中。
  return satp;
}

//
// 为返回用户空间设置陷入帧和控制寄存器
//
void
prepare_return(void)
{
  struct proc *p = myproc();

  // 即将把陷入的处理目标从 kerneltrap() 改为
  // usertrap()。由于内核代码陷入到 usertrap 会
  // 造成灾难性后果，所以先关闭中断。
  intr_off();

  // 把系统调用、中断和异常交给 trampoline.S 中的 uservec
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // 设置 uservec 在进程下次陷入内核时
  // 会用到的陷入帧字段。
  p->trapframe->kernel_satp = r_satp();         // 内核页表
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // 进程的内核栈
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp(); // 供 cpuid() 使用的 hartid

  // 设置 trampoline.S 的 sret 返回
  // 用户空间时要用到的寄存器。

  // 把 S 模式的上一次特权级设置为用户态。
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // 用户态下把 SPP 清零
  x |= SSTATUS_SPIE; // 在用户态使能中断
  w_sstatus(x);

  // 把 S 异常程序计数器设置为保存的用户 pc。
  w_sepc(p->trapframe->epc);
}

// 来自内核代码的中断和异常经 kernelvec 进入这里，
// 使用的是当前的内核栈。
void
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0) {
    // 来自未知来源的中断或陷入
    printk("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(),
           r_stval());
    panic("kerneltrap");
  }

  // 如果是定时器中断，就让出 CPU。
  if (which_dev == 2 && myproc() != 0)
    yield();

  // yield() 可能已经引发了新的陷入，
  // 所以恢复这些陷入寄存器，供 kernelvec.S 的 sepc 指令使用。
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if (cpuid() == 0) {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // 请求下一次定时器中断。这同时会清除
  // 本次中断请求。1000000 约等于
  // 十分之一秒。
  w_stimecmp(r_time() + 1000000);
}

// 检查是外部中断还是软件中断，
// 并处理它。
// 若是定时器中断返回 2，
// 若是其他设备返回 1，
// 若无法识别返回 0。
int
devintr()
{
  uint64 scause = r_scause();

  if (scause == 0x8000000000000009L) {
    // 这是经 PLIC 的 S 模式外部中断。

    // irq 指出是哪个设备产生了中断。
    int irq = plic_claim();

    if (irq == UART0_IRQ) {
      uartintr();
    } else if (irq == VIRTIO0_IRQ) {
      virtio_disk_intr();
    } else if (irq) {
      printk("unexpected interrupt irq=%d\n", irq);
    }

    // PLIC 允许每个设备一次最多只能
    // 提出一个中断；现在告诉 PLIC 该设备
    // 可以再次中断了。
    if (irq)
      plic_complete(irq);

    return 1;
  } else if (scause == 0x8000000000000005L) {
    // 定时器中断。
    clockintr();
    return 2;
  } else {
    return 0;
  }
}
