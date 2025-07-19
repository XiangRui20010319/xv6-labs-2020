// struct buf {
//   int valid;   // has data been read from disk? 表示缓冲区是否包含块的副本
//   int disk;    // does disk "own" buf? 表示缓冲区内容是否已交给磁盘
//   uint dev;    // 设备号（比如硬盘编号）
//   uint blockno;// 磁盘块号（block number）
//   struct sleeplock lock;
//   uint refcnt; // 引用计数
//   struct buf *prev; // LRU cache list
//   struct buf *next;
//   uchar data[BSIZE];
// };

struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  // struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];

  uint lastuse;     //用于跟踪LRU-buf
};