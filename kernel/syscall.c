#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// 从当前进程读取 addr 处的 uint64 值。
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if (addr >= p->sz ||
      addr + sizeof(uint64) > p->sz) // 两个判断都需要，以防溢出
    return -1;
  if (copyin(p->pagetable, p->sz, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// 从当前进程读取 addr 处以 nul 结尾的字符串。
// 返回字符串长度（不含 nul），出错返回 -1。
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if (copyinstr(p->pagetable, p->sz, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// 读取第 n 个 32 位系统调用参数。
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

// 把参数当作指针取出。
// 不检查其合法性，因为
// copyin/copyout 会做这项检查。
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

// 读取第 n 个机器字长的系统调用参数，将其作为以 null 结尾的字符串。
// 复制到 buf，最多 max 字节。
// 成功时返回字符串长度（不含 nul），出错返回 -1。
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);
}

// 处理系统调用的各函数的原型声明。
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_pause(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);
extern uint64 sys_sync(void);

// ---- Agent-OS 扩展 ----
extern uint64 sys_agent_create(void);
extern uint64 sys_agent_info(void);
extern uint64 sys_tool_call(void);
extern uint64 sys_tool_list(void);
extern uint64 sys_context_push(void);
extern uint64 sys_context_query(void);
extern uint64 sys_context_rollback(void);
extern uint64 sys_context_clear(void);
extern uint64 sys_agent_heartbeat_set(void);
extern uint64 sys_agent_heartbeat_stop(void);
extern uint64 sys_agent_watch(void);
extern uint64 sys_agent_wait(void);
extern uint64 sys_agent_unwatch(void);
extern uint64 sys_file_setattr(void);
extern uint64 sys_file_getattr(void);
extern uint64 sys_file_query(void);

// 把 syscall.h 中的系统调用号映射到
// 处理该系统调用的函数的数组。
static uint64 (*syscalls[])(void) = {
  // clang-format off
  [SYS_fork]    = sys_fork,
  [SYS_exit]    = sys_exit,
  [SYS_wait]    = sys_wait,
  [SYS_pipe]    = sys_pipe,
  [SYS_read]    = sys_read,
  [SYS_kill]    = sys_kill,
  [SYS_exec]    = sys_exec,
  [SYS_fstat]   = sys_fstat,
  [SYS_chdir]   = sys_chdir,
  [SYS_dup]     = sys_dup,
  [SYS_getpid]  = sys_getpid,
  [SYS_sbrk]    = sys_sbrk,
  [SYS_pause]   = sys_pause,
  [SYS_uptime]  = sys_uptime,
  [SYS_open]    = sys_open,
  [SYS_write]   = sys_write,
  [SYS_mknod]   = sys_mknod,
  [SYS_unlink]  = sys_unlink,
  [SYS_link]    = sys_link,
  [SYS_mkdir]   = sys_mkdir,
  [SYS_close]   = sys_close,
  [SYS_sync]    = sys_sync,
  // clang-format on
  [SYS_agent_create] = sys_agent_create,
  [SYS_agent_info] = sys_agent_info,
  [SYS_tool_call] = sys_tool_call,
  [SYS_tool_list] = sys_tool_list,
  [SYS_context_push] = sys_context_push,
  [SYS_context_query] = sys_context_query,
  [SYS_context_rollback] = sys_context_rollback,
  [SYS_context_clear] = sys_context_clear,
  [SYS_agent_heartbeat_set] = sys_agent_heartbeat_set,
  [SYS_agent_heartbeat_stop] = sys_agent_heartbeat_stop,
  [SYS_agent_watch] = sys_agent_watch,
  [SYS_agent_wait] = sys_agent_wait,
  [SYS_agent_unwatch] = sys_agent_unwatch,
  [SYS_file_setattr] = sys_file_setattr,
  [SYS_file_getattr] = sys_file_getattr,
  [SYS_file_query] = sys_file_query,
};

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // 用 num 查出处理 num 的系统调用函数，调用它，
    // 并把返回值存入 p->trapframe->a0
    p->trapframe->a0 = syscalls[num]();
  } else {
    printk("%d %s: unknown sys call %d\n", p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
