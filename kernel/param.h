#define NPROC       64                // 最大进程数
#define NCPU        8                 // 最大 CPU 数
#define NOFILE      16                // 每进程可打开的文件数
#define NFILE       100               // 系统可打开的文件数
#define NINODE      50                // 活动索引节点(inode)的最大数量
#define NDEV        10                // 最大主设备号
#define ROOTDEV     1                 // 文件系统根盘的设备号
#define MAXARG      32                // exec 的最大参数个数
#define MAXOPBLOCKS 10                // 单次文件系统操作最多写多少块
#define LOGBLOCKS   (MAXOPBLOCKS * 3) // 磁盘日志中数据块的最大数量
#define NBUF        (MAXOPBLOCKS * 3) // 磁盘块缓存的大小
#define FSSIZE      2000              // 文件系统的块数
#define MAXPATH     128               // 文件路径名的最大长度
#define USERSTACK   1                 // 用户栈页数
