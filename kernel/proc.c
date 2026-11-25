#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// 有助于保证对处于 wait() 中的父进程的唤醒
// 不会丢失。有助于在使用 p->parent 时
// 遵守内存模型。
// 必须在任何 p->lock 之前获取。
struct spinlock wait_lock;

// 为每个进程的内核栈分配一页。
// 把它映射在内存的高地址处，后面紧跟一个
// 无效的保护页(guard page)。
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// 初始化进程表。
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// 调用时必须关闭中断，
// 以防止与「进程被迁移到另一个 CPU」
// 产生竞态。
int
cpuid()
{
  int id = r_tp();
  return id;
}

// 返回本 CPU 的 cpu 结构体。
// 必须已关闭中断。
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// 返回当前的 struct proc *，若没有则返回 0。
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// 在进程表中查找一个 UNUSED 的 proc。
// 若找到，初始化在内核中运行所需的状态，
// 并在返回时保持持有 p->lock。
// 若没有空闲 proc，或内存分配失败，返回 0。
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // 分配一页陷入帧(trapframe)。
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 一张空的用户页表。
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 建立新的上下文(context)，使其从 forkret 开始执行，
  // 而 forkret 会返回用户空间。
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// 释放一个 proc 结构体及挂在其上的数据，
// 包括用户页。
// 必须持有 p->lock。
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable) {
    proc_drop_agent(p, p->pagetable);
    proc_freepagetable(p->pagetable, p->sz);
  }
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// 调用者持有 p->lock。在 exec 时，pagetable 是旧的用户页表。
void
proc_drop_agent(struct proc *p, pagetable_t pagetable)
{
  if (p->agent.quota != 0)
    uvmunmap(pagetable, AGENT_CONTEXT_BASE, p->agent.quota / PGSIZE, 1);
  memset(&p->agent, 0, sizeof(p->agent));
  memset(p->agent_lengths, 0, sizeof(p->agent_lengths));
  memset(&p->agent_timing, 0, sizeof(p->agent_timing));
}

// 只由当前进程调用，该进程拥有对其映射的修改权。
uint64
proc_memlimit(struct proc *p)
{
  return p->agent.type == AGENT_TYPE_AGENT ? AGENT_HEAP_LIMIT : TRAPFRAME;
}

int
agent_get_info(int pid, struct agent_info *out)
{
  struct proc *self = myproc();
  int result = -1;

  acquire(&wait_lock);
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p != self && p->parent != self)
      continue;
    acquire(&p->lock);
    if (p->pid == pid && p->agent.type == AGENT_TYPE_AGENT) {
      *out = p->agent;
      if (p != self)
        out->context_base = 0;
      result = 0;
      release(&p->lock);
      break;
    }
    release(&p->lock);
  }
  release(&wait_lock);
  return result;
}

// 为给定进程创建一张用户页表，其中不含用户内存，
// 但包含跳板页(trampoline)和陷入帧页。
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // 一张空的页表。
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // 把跳板页代码（用于系统调用返回）
  // 映射到最高的用户虚拟地址。
  // 只有内核在进出用户空间的路上使用它，
  // 因此不带 PTE_U。
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // 把陷入帧页映射到跳板页正下方，
  // 供 trampoline.S 使用。
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// 释放进程的页表，并释放它引用的
// 物理内存。
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// 建立第一个用户进程。
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// 按 n 字节增大或缩小用户内存。
// 成功返回 0，失败返回 -1。
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    if (sz + n > proc_memlimit(p)) {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// 创建一个新进程，复制父进程。
// 建立子进程的内核栈，使其返回时如同从 fork() 系统调用返回。
static int
forkproc(int type, int interval, uint64 quota)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  if (type == AGENT_TYPE_AGENT && p->sz > AGENT_HEAP_LIMIT)
    return -1;

  // 分配进程。
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // 把用户内存从父进程复制给子进程。
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  if (type == AGENT_TYPE_AGENT) {
    // 模块 A：Context 区改为"预留 + 惰性批量提交"，
    // 不再在 fork 时一次性提交 quota（默认 16 页），
    // 由 agent_ctx.c 在 Context 区首次访问时按 4 页一批映射。
    np->agent = (struct agent_info){
      .pid = np->pid,
      .type = type,
      .heartbeat_interval = interval,
      .loop_state = AGENT_LOOP_IDLE,
      .context_base = AGENT_CONTEXT_BASE,
      .quota = quota,
    };
  }

  // 复制保存的用户寄存器。
  *(np->trapframe) = *(p->trapframe);

  // 使 fork 在子进程中返回 0。
  np->trapframe->a0 = 0;

  // 增加已打开文件描述符的引用计数(ref count)。
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  if (interval > 0)
    acquire(&tickslock);
  acquire(&np->lock);
  if (interval > 0)
    np->agent_timing.heartbeat_tick = ticks;
  np->state = RUNNABLE;
  release(&np->lock);
  if (interval > 0)
    release(&tickslock);

  return pid;
}

int
kfork(void)
{
  return forkproc(AGENT_TYPE_NORMAL, 0, 0);
}

int
kagent_fork(int type, int interval, uint64 quota)
{
  if (type != AGENT_TYPE_AGENT || interval < 0 || quota < PGSIZE ||
      quota > AGENT_MAX_QUOTA || quota % PGSIZE != 0)
    return -1;
  return forkproc(type, interval, quota);
}

// 把 p 遗弃的子进程交给 init。
// 调用者必须持有 wait_lock。
void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// 退出当前进程。不返回。
// 已退出的进程保持僵尸(zombie)状态，
// 直到其父进程调用 wait()。
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // 关闭所有已打开的文件。
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // 把子进程交给 init。
  reparent(p);

  // 父进程可能正睡眠在 wait() 中。
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // 跳入调度器，永不返回。
  sched();
  panic("zombie exit");
}

// 等待一个子进程退出并返回其 pid。
// 若本进程没有子进程，返回 -1。
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    // 扫描进程表，查找已退出的子进程。
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent == p) {
        // 确保子进程不在 exit() 或 swtch() 中。
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE) {
          // 找到一个。
          pid = pp->pid;
          if (addr != 0 &&
              copyout(p->pagetable, p->sz, addr, (char *)&pp->xstate,
                      sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          pp->parent = 0;
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // 若没有任何子进程，等待没有意义。
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    // 等待一个子进程退出。
    sleep_prepare(p); //DOC: wait-sleep
    release(&wait_lock);
    sleep();
    acquire(&wait_lock);
  }
}

// 每核(per-CPU)进程调度器(scheduler)。
// 每个 CPU 在完成自身初始化后调用 scheduler()。
// 调度器从不返回。它循环执行：
//  - 选择一个要运行的进程。
//  - swtch 到该进程以开始运行它。
//  - 最终该进程通过 swtch 把控制权
//    交回调度器。
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // 最近运行的那个进程可能关闭了中断；
    // 先打开中断，以免所有进程都在等待时死锁。
    // 随后再把中断关闭，
    // 以避免中断与 wfi 之间
    // 可能的竞态。
    intr_on();
    intr_off();

    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        // 切换到选中的进程。释放自己的锁、
        // 并在跳回这里之前重新获取它，
        // 是该进程自己的职责。
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // 释放时不要重新打开中断。
        mycpu()->intena = 0;

        // 该进程本次运行到此结束。
        // 它应当在返回前已经改好自己的 p->state。
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0) {
      // 没有可运行的进程；在本核上停机直到有中断。
      asm volatile("wfi");
    }
  }
}

// 切换到调度器。必须只持有 p->lock
// 并且已经改好 proc->state。保存并恢复
// intena，因为 intena 是本内核线程的属性，
// 而不是本 CPU 的属性。它本应是
// proc->intena 和 proc->noff，但那样会在
// 少数「持有锁但没有进程」的地方
// 出问题。
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// 让出一个调度轮次的 CPU。
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// fork 出的子进程第一次被 scheduler() 调度时，
// 会 swtch 到 forkret。
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // 仍然持有来自调度器的 p->lock。
  release(&p->lock);

  if (first) {
    first = 0;

    // 文件系统初始化必须在普通进程的上下文(context)中运行
    // （例如因为它会调用 sleep），因此不能
    // 从 main() 中运行。
    fsinit(ROOTDEV);

    // 文件系统已初始化，现在可以调用 kexec() 了。
    // 把 kexec 的返回值 (argc) 放入 a0。
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // 返回用户空间，模拟 usertrap() 的返回过程。
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// 把当前进程登记为在 chan 上等待唤醒。
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}

// 让线程睡眠。假定此前已调用 sleep_prepare()。
// 若在此期间 sleep_prepare() 登记的通道已被唤醒，
// 则不进入睡眠，而是立即返回。
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
}

// 唤醒所有在通道 chan 上睡眠的进程。
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      // 若该进程正在此通道上等待唤醒，
      // 则通过清除 p->chan 来表明唤醒已经发生。
      p->chan = 0;

      // 若该等待进程已经走到真正进入睡眠这一步，
      // 还要把它重新置为 RUNNING。
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
      }
    }
    release(&p->lock);
  }
}

// 计时路径统一先拿 tickslock，再拿 p->lock。
int
agent_heartbeat_configure(int interval)
{
  struct proc *p = myproc();

  acquire(&tickslock);
  acquire(&p->lock);
  if (p->agent.type != AGENT_TYPE_AGENT) {
    release(&p->lock);
    release(&tickslock);
    return -1;
  }
  p->agent.heartbeat_interval = interval;
  p->agent_timing.heartbeat_tick = ticks;
  p->agent_timing.heartbeat_pending = 0;
  release(&p->lock);
  release(&tickslock);
  return 0;
}

int
agent_wait_event(int timeout)
{
  struct proc *p = myproc();
  struct agent_timing *timing = &p->agent_timing;
  int result = -1;

  if (timeout < AGENT_WAIT_FOREVER)
    return -1;
  acquire(&tickslock);
  acquire(&p->lock);
  if (p->agent.type != AGENT_TYPE_AGENT || p->killed) {
    release(&p->lock);
    release(&tickslock);
    return -1;
  }
  if (timing->heartbeat_pending) {
    timing->heartbeat_pending = 0;
    result = AGENT_EVENT_HEARTBEAT;
  } else if (timeout != 0) {
    timing->wait_tick = ticks;
    timing->wait_timeout = timeout;
    timing->wait_result = -1;
    p->chan = timing;
    p->agent.loop_state = AGENT_LOOP_WAITING;
    p->state = SLEEPING;

    // 检查待处理事件、登记通道和进入 SLEEPING 都持有 p->lock。
    // sched 只允许持有进程锁。定时器随后可取得该锁并定向唤醒。
    release(&tickslock);
    sched();

    result = p->killed ? -1 : timing->wait_result;
    p->chan = 0;
    p->agent.loop_state = AGENT_LOOP_IDLE;
    timing->wait_timeout = 0;
    release(&p->lock);
    return result;
  }
  release(&p->lock);
  release(&tickslock);
  return result;
}

// 只有 hart 0 持有 tickslock 并推进全局时钟时调用。
void
agent_clock_tick(uint now)
{
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state != UNUSED && p->state != USED && p->state != ZOMBIE &&
        p->agent.type == AGENT_TYPE_AGENT && !p->killed) {
      struct agent_timing *timing = &p->agent_timing;
      uint interval = p->agent.heartbeat_interval;
      uint elapsed = now - timing->heartbeat_tick;

      if (interval != 0 && elapsed >= interval) {
        // 保持周期相位。等待者来不及消费的多次心跳合并为一次。
        timing->heartbeat_tick += (elapsed / interval) * interval;
        timing->heartbeat_pending = 1;
      }
      if (p->state == SLEEPING && p->chan == timing) {
        if (timing->heartbeat_pending) {
          timing->heartbeat_pending = 0;
          timing->wait_result = AGENT_EVENT_HEARTBEAT;
          p->state = RUNNABLE;
        } else if (timing->wait_timeout > 0 &&
                   now - timing->wait_tick >= (uint)timing->wait_timeout) {
          p->state = RUNNABLE;
        }
      }
    }
    release(&p->lock);
  }
}

// 杀死具有给定 pid 的进程。
// 受害进程要等到它尝试返回用户空间时才会退出
// （见 trap.c 中的 usertrap()）。
int
kkill(int pid)
{
  struct proc *p;

  if (pid == 0)
    return -1;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        // 把进程从 sleep() 中唤醒。
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// 复制到用户地址或内核地址，
// 取决于 usr_dst。
// 成功返回 0，出错返回 -1。
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// 从用户地址或内核地址复制，
// 取决于 usr_src。
// 成功返回 0，出错返回 -1。
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// 把进程列表打印到控制台。用于调试。
// 用户在控制台输入 ^P 时运行。
// 不加锁，以免把已经卡住的机器进一步卡死。
void
procdump(void)
{
  static char *states[] = {
    // clang-format off
    [UNUSED]    = "unused",
    [USED]      = "used",
    [SLEEPING]  = "sleep ",
    [RUNNABLE]  = "runble",
    [RUNNING]   = "run   ",
    [ZOMBIE]    = "zombie"
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\n");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printk("%d %s %s", p->pid, state, p->name);
    printk("\n");
  }
}
