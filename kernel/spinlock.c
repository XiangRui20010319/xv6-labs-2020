// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
/*
条件编译
只有在编译时定义了 LAB_LOCK 宏，接下来的代码段才会被编译；否则这段代码将被忽略。
#ifdef LAB_LOCK
……
#endif
*/
#ifdef LAB_LOCK
#define NLOCK 500

static struct spinlock *locks[NLOCK];
struct spinlock lock_locks;

void
freelock(struct spinlock *lk)
{
  acquire(&lock_locks);
  int i;
  for (i = 0; i < NLOCK; i++) {
    if(locks[i] == lk) {
      locks[i] = 0;
      break;
    }
  }
  release(&lock_locks);
}

static void
findslot(struct spinlock *lk) {
  acquire(&lock_locks);
  int i;
  for (i = 0; i < NLOCK; i++) {
    if(locks[i] == 0) {
      locks[i] = lk;
      release(&lock_locks);
      return;
    }
  }
  panic("findslot");
}
#endif

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
#ifdef LAB_LOCK
  lk->nts = 0;
  lk->n = 0;
  findslot(lk);
#endif  
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
acquire(struct spinlock *lk)
{
  push_off(); // disable interrupts to avoid deadlock.  一些xv6自旋锁保护线程和中断处理程序共用的数据，因此xv6在获取锁时，直接禁止该CPU上的中断。
  if(holding(lk))
    panic("acquire");

#ifdef LAB_LOCK
    __sync_fetch_and_add(&(lk->n), 1);
#endif      

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0) { // 原地自旋，返回的是&lk->locked交换前的值，只有为0，即获得了锁
#ifdef LAB_LOCK
    __sync_fetch_and_add(&(lk->nts), 1);
#else
   ;
#endif
  }

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  /*
  是一个内存屏障（memory barrier）或称为内存栅栏，它的作用是告诉编译器和CPU：不要对这行之前和之后的内存访问做乱序优化
  */
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu(); // 记录下来获取锁的CPU
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);

  pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.
/*
push_off() 和 pop_off() 保证在临界区中关闭中断，并支持嵌套调用；
在退出最后一层嵌套时才恢复中断状态，且恢复前确保中断最初是开启的。
嵌套是因为（比如可能在函数 A 里关一次，B 再次关一次）
*/
void
push_off(void)
{
  int old = intr_get();    // 返回当前 CPU 中断是否开启。

  intr_off();              // 关闭中断，防止中断打断临界区操作。
  if(mycpu()->noff == 0)   // 如果这是第一次调用 push_off, mycpu()->noff是当前 CPU 中关闭中断的嵌套层数
    mycpu()->intena = old; // 记录原来的中断状态
  mycpu()->noff += 1;      // 嵌套层数 +1
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if(intr_get())                       // 若中断居然是开的，说明代码出错了
    panic("pop_off - interruptible");
  if(c->noff < 1)                      // 如果嵌套层数 < 1，说明 pop_off 调用次数过多
    panic("pop_off");
  c->noff -= 1;                        // 嵌套层数 -1
  if(c->noff == 0 && c->intena)        // 如果这是最后一次 pop_off，并且最初中断是开的，则恢复中断
    intr_on();
}

#ifdef LAB_LOCK
int
snprint_lock(char *buf, int sz, struct spinlock *lk)
{
  int n = 0;
  if(lk->n > 0) {
    n = snprintf(buf, sz, "lock: %s: #fetch-and-add %d #acquire() %d\n",
                 lk->name, lk->nts, lk->n);
  }
  return n;
}

int
statslock(char *buf, int sz) {
  int n;
  int tot = 0;

  acquire(&lock_locks);
  n = snprintf(buf, sz, "--- lock kmem/bcache stats\n");
  for(int i = 0; i < NLOCK; i++) {
    if(locks[i] == 0)
      break;
    if(strncmp(locks[i]->name, "bcache", strlen("bcache")) == 0 ||
       strncmp(locks[i]->name, "kmem", strlen("kmem")) == 0) {
      tot += locks[i]->nts;
      n += snprint_lock(buf +n, sz-n, locks[i]);
    }
  }
  
  n += snprintf(buf+n, sz-n, "--- top 5 contended locks:\n");
  int last = 100000000;
  // stupid way to compute top 5 contended locks
  for(int t = 0; t < 5; t++) {
    int top = 0;
    for(int i = 0; i < NLOCK; i++) {
      if(locks[i] == 0)
        break;
      if(locks[i]->nts > locks[top]->nts && locks[i]->nts < last) {
        top = i;
      }
    }
    n += snprint_lock(buf+n, sz-n, locks[top]);
    last = locks[top]->nts;
  }
  n += snprintf(buf+n, sz-n, "tot= %d\n", tot);
  release(&lock_locks);  
  return n;
}
#endif
