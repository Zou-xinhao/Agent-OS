#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

// 所有 CPU 都从 start() 跳到这里，此时已处于 S 模式。
void
main()
{
  if (cpuid() == 0) {
    consoleinit();
    printkinit();
    printk("\n");
    printk("xv6 kernel is booting\n");
    printk("\n");
    kinit();            // 物理页分配器
    kvminit();          // 建立内核页表
    kvminithart();      // 开启分页
    procinit();         // 进程表
    trapinit();         // 陷入向量
    trapinithart();     // 安装内核陷入向量
    plicinit();         // 设置中断控制器
    plicinithart();     // 向 PLIC 申请设备中断
    binit();            // 缓冲区缓存
    iinit();            // 索引节点(inode)表
    fileinit();         // 文件表
    virtio_disk_init(); // 模拟硬盘
    userinit();         // 第一个用户进程

    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
  } else {
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) == 0)
      ;

    printk("hart %d starting\n", cpuid());
    kvminithart();  // 开启分页
    trapinithart(); // 安装内核陷入向量
    plicinithart(); // 向 PLIC 申请设备中断
  }

  scheduler();
}
