// Mutual exclusion lock. 自旋锁
struct spinlock {
  uint locked;       // Is the lock held? 当锁可用时为零，当它被持有时为非零

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

