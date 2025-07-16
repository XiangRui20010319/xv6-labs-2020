#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time"); // 初始化一个自旋锁 tickslock，并为其命名为 "time"
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec); // 将 kernelvec 的地址写入 stvec 寄存器，从而设置中断和异常的入口处理函数为 kernelvec
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0; // 记录是哪个设备触发了中断

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec); // 将 kernelvec 的地址写入 stvec 寄存器，从而设置中断和异常的入口处理函数为 kernelvec。

  struct proc *p = myproc(); // 获取当前正在运行的进程结构指针。
  
  // save user program counter.
  p->trapframe->epc = r_sepc(); // 保存用户态 EPC（异常程序计数器），用于从 trap 返回时恢复用户态执行的位置。
  
  if(r_scause() == 8){  //  判断当前 trap 是否是系统调用（8 表示 `ecall from U-mode`）
    // system call

    if(p->killed) // 如果进程已经被标记为被杀死了，立刻退出
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4; // RISC-V 指令是定长的，每条指令占 4 字节（32 位）

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    // 打开中断，调用 syscall() 函数处理系统调用
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){ // - 如果是外部中断（如时钟、串口、磁盘），进入设备中断处理函数 `devintr()`。- 如果返回值非零，说明确实是某个设备中断（2 代表时钟）
    // ok
  } else { // 如果既不是系统调用也不是设备中断，那就是未知 trap，打印调试信息，杀掉当前进程。
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  // if(which_dev == 2) // 如果是 时钟中断（编号 2），说明到了时间片，该让出 CPU，执行 yield() 进入调度器
  //   yield();

  // 第三个实验
  if (which_dev == 2) {
      if (p->kama_alarm_interval != 0 && --p->kama_alarm_ticks <= 0 && p->kama_alarm_goingoff == 0) {
      	  // 是否设置了时钟 && 时钟倒计时是否结束 && 没有其他时钟正在运行
          // 如果一个时钟到期的时候已经有一个时钟处理函数正在运行，
          // 则会推迟到原处理函数运行完成后的下一个 tick 才触发这次时钟
          p->kama_alarm_ticks = p->kama_alarm_interval;      // 重置时钟倒计时
          *p->kama_alarm_trapframe = *p->trapframe;          // 保存当前进程陷阱帧
          p->trapframe->epc = (uint64)p->kama_alarm_handler; // 跳转到时钟回调函数
          p->kama_alarm_goingoff = 1;                        // 标记当前已有时钟正在运行
      }
    yield();
  }

  usertrapret(); // 从内核态返回用户态
}

//
// return to user space 总结一下：“准备跳回用户态运行，并告诉系统下次 trap（系统调用或中断）该如何重新回到内核”。
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off(); //  禁用中断，避免切换过程中打断

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline)); // 将 stvec 设置为 uservec 入口地址。

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel. 这些信息是为下一次用户态切换回内核时恢复上下文所需。
  p->trapframe->kernel_satp = r_satp();         // kernel page table 当前页表
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack 进程内核栈顶
  p->trapframe->kernel_trap = (uint64)usertrap; // trap 入口
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid() 当前 CPU 编号

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User. 修改 sstatus 状态寄存器，准备返回用户态
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode // SPP = 0 → 表示下一次 sret 将切换到用户模式
  x |= SSTATUS_SPIE; // enable interrupts in user mode // 允许用户态开启中断
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.  // 设置 sepc，表示返回用户态后从哪里执行
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to. // 准备进入 trampoline.S，设置页表
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret. 最关键一步：跳入 trampoline.S 中的 userret
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is. 内核中断/异常处理函数, 它在 CPU 处于 内核态（supervisor mode） 时被调用
void 
kerneltrap()
{
  //  保存 trap 时的关键信息, 这些值稍后会被还原，确保 trap 处理完后能继续执行被中断的内核代码。
  int which_dev = 0;
  uint64 sepc = r_sepc(); // 保存 trap 时的程序计数器
  uint64 sstatus = r_sstatus(); // 保存 trap 时的 sstatus 状态寄存器
  uint64 scause = r_scause(); // 保存 trap 的原因（中断类型）
  
  if((sstatus & SSTATUS_SPP) == 0) // SPP 应该是 1，表示上一次是内核态 trap
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0) // 如果当前处于中断打开状态，也会 panic
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt. 若是定时器中断并且当前进程正在运行 → 让出 CPU
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction. 恢复寄存器值,确保能继续内核中的原始代码执行。
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks); // 唤醒所有正在等待 ticks 地址处睡眠的进程。
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt, 软件定时器中断（timer interrupt）
// 1 if other device, 外部设备中断（如 UART、磁盘）
// 0 if not recognized. 
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) && // 判断是否为外部中断（PLIC 管理）
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim(); // 向 PLIC 查询是哪一个设备产生的中断

    if(irq == UART0_IRQ){ // 处理串口输入
      uartintr();
    } else if(irq == VIRTIO0_IRQ){  // 虚拟磁盘中断处理
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq); // PLIC 机制：每个设备只能发起一次中断，必须调用 plic_complete 告知“我处理完了”，才能允许再次中断。

    return 1;
  } else if(scause == 0x8000000000000001L){ // 软件中断（定时器中断）
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){ // 时钟中断只由 CPU 0 处理
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2); // 清除 sip 的 SSIP（Supervisor Software Interrupt Pending）位，表示中断已处理完。

    return 2;
  } else {
    return 0;
  }
}

// 第三个实验
// 设置进程中时钟的相关属性
int kama_sigalarm(int ticks, void(*handler)()) {
    struct proc* p = myproc();
    p->kama_alarm_interval = ticks;
    p->kama_alarm_handler = handler;
    p->kama_alarm_ticks = ticks;
    return 0;
}

//将进程恢复到alarm中断前的状态
int kama_sigreturn() {
    struct proc* p = myproc();
    *p->trapframe = *p->kama_alarm_trapframe;
    p->kama_alarm_goingoff = 0;
    return 0;
}