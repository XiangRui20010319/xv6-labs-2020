#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n; // 记录日志中数据块的数量。
  int block[LOGSIZE]; // 存放 n 个数据块在磁盘中的编号
};

struct log {
  struct spinlock lock;
  int start;     // 日志在磁盘上开始的位置（块号）。
  int size;     //  日志占用的磁盘块总数。决定了日志空间的大小。
  int outstanding; // how many FS sys calls are executing. 当前有多少个文件系统的系统调用（比如写文件）还没完成。只要还有正在进行的系统调用，就不能提交日志。
  int committing;  // in commit(), please wait. 标记当前是否正在执行 commit() 操作。其他线程看到这个标志，会等待当前提交完成后再进行。
  int dev;
  struct logheader lh; // 日志头部，记录了哪些块被修改了（通常是一个块号数组），用于在崩溃恢复时重新写回。
};
struct log log;

static void recover_from_log(void);
static void commit();

void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.size = sb->nlog;
  log.dev = dev;
  recover_from_log();
}

// Copy committed blocks from log to their home location
/*
将日志中已经提交的块（committed blocks）复制回它们原本在磁盘上的位置
*/
static void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    bwrite(dbuf);  // write dst to disk
    if(recovering == 0) // 0说明当前不是在崩溃恢复阶段
      bunpin(dbuf);
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
/*
将磁盘日志头（log header）读入内存中的日志头结构。
“我们从硬盘上读取一页‘日志头说明书’，然后把说明书中的内容（有几个日志块、每个块指向哪里）记到内存里的备忘录中，以便后续恢复或提交。”
*/
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);  // 这个块是日志头在磁盘上的位置。
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf); // 释放进程对某个缓存块（struct buf *b）的使用权

}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
/*
将内存中的日志头写回磁盘，标志当前事务真正提交（commit）成功
*/
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

static void
recover_from_log(void)
{
  read_head();
  install_trans(1); // if committed, copy from log to disk
  log.lh.n = 0;
  write_head(); // clear the log
}

// called at the start of each FS system call.
/*
begin_op():
    🔒 加锁
    🔁 一直检查：
        🟥 如果日志正在提交，等一会儿（sleep）
        🟧 如果日志快满了，也等（sleep）
        ✅ 如果没在提交，空间也够：
            ☑️ 增加事务数
            🔓 解锁
            ✅ 开始执行文件系统调用
*/
void
begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){
      sleep(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){   // 日志空间可能不够，每个系统调用最多可以写入MAXOPBLOCKS个不同的块。
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
/*
表示一次文件系统事务结束, 如果这是最后一个正在执行的操作，就调用 commit() 来持久化日志（写入磁盘）
*/
void
end_op(void)
{
  int do_commit = 0;        // 用于判断是否要执行提交

  acquire(&log.lock);
  log.outstanding -= 1;
  if(log.committing)        //  如果已经在提交，说明逻辑出错
    panic("log.committing");
  if(log.outstanding == 0){ // 如果这是最后一个系统调用，需要提交日志
    do_commit = 1;
    log.committing = 1;
  } else {                  // 如果不是最后一个，那就唤醒可能在 begin_op() 中等待的其他线程
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log);
  }
  release(&log.lock);

  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
/*
将事务中修改的每个块从缓冲区缓存复制到磁盘上日志槽位中。
*/
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to = bread(log.dev, log.start+tail+1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    write_log();     // Write modified blocks from cache to log
    write_head();    // Write header to disk -- the real commit
    install_trans(0); // Now install writes to home locations
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
/*
log_write() 并不立刻将数据写入磁盘，而是告诉日志系统：“我改动了这个块（b->data），请记下来，等 commit 的时候统一写入”。

就像购物时把你买的商品放入“购物车”，最后结账时再统一付款（commit）。
*/
void
log_write(struct buf *b)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  if (log.outstanding < 1) // 确保这次 log_write() 是在一个事务（begin_op() ~ end_op()）内部调用的
    panic("log_write outside of trans");

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++) {    // 判断当前 block 是否已在日志中（log absorbtion 吸收机制）
    if (log.lh.block[i] == b->blockno)   // log absorbtion
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n) {  // Add new block to log?
    bpin(b);
    log.lh.n++;
  }
  release(&log.lock);
}

