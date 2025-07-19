// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// 哈希表中的桶号索引
#define NBUFMAP_BUCKET 13
// 哈希索引
#define BUFMAP_HASH(dev, blockno) ((((dev)<<27)|(blockno))%NBUFMAP_BUCKET)

struct {
//   struct spinlock lock;
  struct buf buf[NBUF];
  struct spinlock eviction_lock;        // 驱逐锁
  // 哈希表
  struct buf bufmap[NBUFMAP_BUCKET];
  struct spinlock bufmap_locks[NBUFMAP_BUCKET];     // 桶锁
} bcache;

// struct {
//   struct spinlock lock;
//   struct buf buf[NBUF]; // NBUF = 10*3 = 30

//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   struct buf head;
// } bcache; // 缓存池

/*
初始化一个双向循环链表的缓存池，其中每个节点是一个 buf 缓冲块，并给每个缓冲块配上了睡眠锁。
你可以把 bcache 想成一个酒店：
bcache.lock 就是酒店大门的锁，只有一个人能进来安排房间（缓冲块）；
bcache.head 是前台登记本，指向所有房间；
每个房间（buf）都有自己的门锁（睡眠锁）；
binit 就是酒店开张前准备好所有房间和钥匙的过程。
*/
// void
// binit(void)
// {
//   struct buf *b;

//   initlock(&bcache.lock, "bcache"); // 初始化整个缓存池的自旋锁

//   // Create linked list of buffers
//   bcache.head.prev = &bcache.head;
//   bcache.head.next = &bcache.head;
//   for(b = bcache.buf; b < bcache.buf+NBUF; b++){ // head <-> buf[NBUF-1] <-> ... <-> buf[1] <-> buf[0] <-> head
//     b->next = bcache.head.next;
//     b->prev = &bcache.head;
//     initsleeplock(&b->lock, "buffer");
//     bcache.head.next->prev = b;
//     bcache.head.next = b;
//   }
// }

void
binit(void)
{
    // 初始化桶锁
    for (int i = 0;i < NBUFMAP_BUCKET;++i) {
        initlock(&bcache.bufmap_locks[i], "bcache_bufmap");
        bcache.bufmap[i].next = 0;
    }

    for (int i = 0;i < NBUF;++i) {
        // 初始化缓存区块
        struct buf* b = &bcache.buf[i];
        initsleeplock(&b->lock, "buffer");
        b->lastuse = 0;
        b->refcnt = 0;

        // 将所有缓存区块添加到bufmap[0]
        b->next = bcache.bufmap[0].next;
        bcache.bufmap[0].next = b;
    }

    initlock(&bcache.eviction_lock, "bcache_eviction");
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
/*
从 缓冲区缓存（buffer cache）中获取一个指定磁盘块的缓冲区（struct buf）
*/
// static struct buf*
// bget(uint dev, uint blockno)
// {
//   struct buf *b;

//   acquire(&bcache.lock);

//   // Is the block already cached?
//   for(b = bcache.head.next; b != &bcache.head; b = b->next){
//     if(b->dev == dev && b->blockno == blockno){
//       b->refcnt++;
//       release(&bcache.lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }

//   // Not cached. 如果缓存中没有找到：回收一个未使用的缓冲区
//   // Recycle the least recently used (LRU) unused buffer.  LRU 策略：越后面使用得越少
//   for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
//     if(b->refcnt == 0) {
//       b->dev = dev;
//       b->blockno = blockno;
//       b->valid = 0;     // 表示缓冲区不包含了
//       b->refcnt = 1;
//       release(&bcache.lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }
//   panic("bget: no buffers");
// }

static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // 哈希获取桶号
  uint key = BUFMAP_HASH(dev, blockno);

  acquire(&bcache.bufmap_locks[key]);

  // blockno的缓存区块是否已经在缓存区中
  for (b = bcache.bufmap[key].next;b;b = b->next) {
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 不在缓存区

  // 为了防止死锁，先释放当前桶锁
  release(&bcache.bufmap_locks[key]);
  // 为了防止blockno的缓存区块被重复创建，加上驱逐锁
  acquire(&bcache.eviction_lock);

  // 释放桶锁-->加驱逐锁的间隙可能创建了blocknod的缓存区块，因此再检查一次
  for (b = bcache.bufmap[key].next;b;b = b->next) {
      if (b->dev == dev && b->blockno == blockno) {
        acquire(&bcache.bufmap_locks[key]);     // 添加引用次数时必须加上桶锁
        b->refcnt++;
        release(&bcache.bufmap_locks[key]);
        release(&bcache.eviction_lock);
        acquiresleep(&b->lock);
        return b;
    }
  }

  // 仍然不在缓存区
  // 此时只持有驱逐锁，不持有任何桶锁。查询所有桶中的LRU-buf

  struct buf* before_least = 0;     // LRU-buf的前一个块
  uint holding_bucket = -1;         //记录当前持有哪个桶锁

  // 循环查询所有桶
  for (int i = 0;i < NBUFMAP_BUCKET;++i) {
      acquire(&bcache.bufmap_locks[i]);     // 获取当前遍历的桶锁(在找到下一个LRU-buf或驱逐内存之前都不释放)

      int newfound = 0;     // 是否在当前桶找到的新的LRU-buf

      for (b = &bcache.bufmap[i];b->next;b = b->next) {
          if (b->next->refcnt == 0 && (!before_least || b->next->lastuse < before_least->next->lastuse)) {
              before_least = b;
              newfound = 1;
          }
      }
      if (!newfound)                            // 如果没找到找到新的LRU-buf，就释放当前的桶锁
          release(&bcache.bufmap_locks[i]);
      else {                                                    // 找到了新的LRU-buf
          if (holding_bucket != -1)                             // 如果当前找到的不是第一个LRU-buf，之前肯定持有某个桶锁，需要释放  
              release(&bcache.bufmap_locks[holding_bucket]);
          holding_bucket = i;                                   // 把标记 holding_bucket 更改成当前桶锁编号
      }
  }

  // 如果没找到任何一个LRU-buf，表示没有空闲缓存块了
  if (!before_least)
      panic("bget: no buffuers");

  b = before_least->next;           // b=LRU-buf

  if (holding_bucket != key) {      // 想要偷的块如果不在key桶，就要把块从他所在的桶驱逐出来
      before_least->next = b->next;
      release(&bcache.bufmap_locks[holding_bucket]);

      //将LRU-buf添加到key桶
      acquire(&bcache.bufmap_locks[key]);
      b->next = bcache.bufmap[key].next;
      bcache.bufmap[key].next = b;
  }

  // 设置新buf的字段
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  // 可以释放相关锁了
  release(&bcache.bufmap_locks[key]);
  release(&bcache.eviction_lock);
  acquiresleep(&b->lock);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);  // 向磁盘发起 读取 请求，把对应的块读到缓冲块中。
    b->valid = 1;         // 表示该缓冲块现在拥有有效数据。
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1); // 向磁盘发起 读取 请求，把对应的缓冲块内容写到磁盘中。
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
/*
释放进程对某个缓存块（struct buf *b）的使用权
*/
// void
// brelse(struct buf *b)
// {
//   if(!holdingsleep(&b->lock))
//     panic("brelse");

//   releasesleep(&b->lock);

//   acquire(&bcache.lock);
//   b->refcnt--;              // 把缓冲块的引用计数减一，意思是：有一个进程不再用它了。
//   if (b->refcnt == 0) {    // 如果引用数变成了 0，说明已经没有任何进程在使用这个缓存块了。 
//     // no one is waiting for it. 头插法-LRU 最近最少使用的
//     b->next->prev = b->prev;
//     b->prev->next = b->next;
//     b->next = bcache.head.next;
//     b->prev = &bcache.head;
//     bcache.head.next->prev = b;
//     bcache.head.next = b;
//   }
  
//   release(&bcache.lock);
// }

// void
// bpin(struct buf *b) {
//   acquire(&bcache.lock);
//   b->refcnt++;
//   release(&bcache.lock);
// }

// void
// bunpin(struct buf *b) {
//   acquire(&bcache.lock);
//   b->refcnt--;
//   release(&bcache.lock);
// }

void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
      b->lastuse = ticks;
  }
  
  release(&bcache.bufmap_locks[key]);
}

void
bpin(struct buf* b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
    
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt++;
  release(&bcache.bufmap_locks[key]);
}

void
bunpin(struct buf* b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
    
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  release(&bcache.bufmap_locks[key]);
}
