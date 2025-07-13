#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
名称以kvm开头的函数操作内核页表；以uvm开头的函数操作用户页表；其他函数用于二者。
*/

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable; // typedef uint64 *pagetable_t; 512 PTEs

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
// void
// kvminit()
// {
//   kernel_pagetable = (pagetable_t) kalloc(); // kalloc 从操作系统维护的空闲页链表中取出一页内存（如果有），用 0x05 填充后返回其地址；整个过程是线程安全的
//   memset(kernel_pagetable, 0, PGSIZE);

//   // uart registers                   #define UART0 0x10000000L
//   kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W); // 将物理地址 UART0 映射到虚拟地址 UART0，大小为一页 (PGSIZE)，权限为“可读 + 可写”

//   // virtio mmio disk interface       #define VIRTIO0 0x10001000
//   kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

//   // CLINT                            #define CLINT 0x2000000L
//   kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

//   // PLIC                             #define PLIC 0x0c000000L
//   kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);
//   //                                  #define KERNBASE 0x80000000L
//   // map kernel text executable and read-only.
//   kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

//   // map kernel data and the physical RAM we'll make use of.
//   kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

//   // map the trampoline for trap entry/exit to
//   // the highest virtual address in the kernel.
//   kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
// }
void kama_kvm_map_pagetable(pagetable_t pgtbl) {
    // 将各种内核需要的 direct mapping 添加到页表 pgtbl 中
    
    // uart registers
    kvmmap(pgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

    // virtio mmio disk interface
    kvmmap(pgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

    // CLINT
    // kvmmap(pgtbl, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

    // PLIC
    kvmmap(pgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

    // map kernel text executable and read-only.
    kvmmap(pgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

    // map kernel data and the physical RAM we'll make use of.
    kvmmap(pgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

    // map the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    kvmmap(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

pagetable_t
kama_kvminit_newpgtbl()
{
    pagetable_t pgtbl = (pagetable_t) kalloc();
    memset(pgtbl, 0, PGSIZE);

    kama_kvm_map_pagetable(pgtbl);

    return pgtbl;
}
/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
    // 全局内核页表仍然使用kvminit函数来初始化
    kernel_pagetable = kama_kvminit_newpgtbl();
    // 全局内核页表仍需要映射 CLINT
    kvmmap(kernel_pagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable)); // 将页表物理地址 pagetable 转换成 satp 寄存器要求的格式，启用 Sv39 分页模式。
  sfence_vma(); // 刷新整个 TLB（翻译后备缓冲区）
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
/*
walk() 是多级页表查询函数，逐级查找虚拟地址 va 所对应的页表项 pte，
必要时自动分配中间页表，并返回第0级页表项指针，用于最终建立或查询映射关系。
*/
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA) // [0x0000000000 ~ 0x3FFFFFFFFF]  // 2^38 = 256GB 虚拟地址空间
    panic("walk");

  for(int level = 2; level > 0; level--) { // level=2表示根
    pte_t *pte = &pagetable[PX(level, va)]; // 根据va地址中第level层页表的9位索引来索引得到该页表
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V; // 把新分配的页表页地址写入到当前页表项中，并标记为有效（PTE_V）
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
/*
根据页表 pagetable 和虚拟地址 va，查找并返回对应的物理地址（pa），前提是这个页对用户态程序是可访问的（PTE_U）。
*/
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0) // 控制用户模式下的指令是否被允许访问页面
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
// void
// kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) // 将虚拟地址 va 映射到物理地址 pa，大小为一页 (PGSIZE)，并声明权限perm
// {
//   if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
//     panic("kvmmap");
// }
void
kvmmap(pagetable_t pgtbl, uint64 va, uint64 pa, uint64 sz, int perm)     // 将某个虚拟地址映射到某个物理地址（添加第一个参数）       
{
    if(mappages(pgtbl, va, sz, pa, perm) != 0)
        panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
/*
将内核虚拟地址 va 转换为对应的物理地址 pa + offset
*/
// uint64
// kvmpa(uint64 va)
// {
//   uint64 off = va % PGSIZE;
//   pte_t *pte;
//   uint64 pa;
  
//   pte = walk(kernel_pagetable, va, 0);
//   if(pte == 0)
//     panic("kvmpa");
//   if((*pte & PTE_V) == 0)
//     panic("kvmpa");
//   pa = PTE2PA(*pte);
//   return pa+off;
// }

uint64
kvmpa(pagetable_t pgtbl, uint64 va)         // kvmpa 将虚拟地址翻译为物理地址（添加第一个参数）
{
    uint64 off = va % PGSIZE;
    pte_t *pte;
    uint64 pa;

    pte = walk(pgtbl, va, 0);			//kernel_pagetable改为参数pgtbl
    if (pte == 0)
        panic("kvmpa");
    if ((*pte & PTE_V) == 0)
        panic("kvmpa");
    pa = PTE2PA(*pte);
    return pa + off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
/*
mappages() 会将一段虚拟地址 [va, va+size) 映射到对应的物理地址 [pa, pa+size)，按页为单位建立页表项，并附加给定的权限 perm，失败就返回 -1。
*/
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va); // PGROUNDDOWN(a) 将地址 a 向下取整为页大小 PGSIZE 的整数倍，即对齐到页的起始地址。
  last = PGROUNDDOWN(va + size - 1); // 把起始虚拟地址和终止虚拟地址对齐到页边界，确保映射页粒度一致。
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0) // 查找虚拟地址 a 对应的页表项指针 pte。如果页表不存在中间目录项，参数 1 表示会自动创建。
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE; // 移动到下一页
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
/*
uvmunmap() 取消页表中从虚拟地址 va 开始、跨越 npages 页的映射，如果 do_free 为真，则同时释放对应的物理页帧。
*/
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
/*
uvmcreate() 分配一页物理内存作为用户进程页表的根页，并初始化为全 0，返回该页的地址。
*/
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
/*
uvminit() 为用户虚拟地址空间中的 第 0 页 分配物理内存，设置页表映射，并把程序的前 sz 字节从 src 拷贝进去。
*/
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
/*
uvmalloc() 在用户进程的页表中，将虚拟地址空间从 oldsz 扩展到 newsz，并为每一页分配物理内存、建立映射。
*/
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
/*
uvmdealloc() 将用户虚拟地址空间从 oldsz 缩小到 newsz，取消并释放不再使用的页（非页对齐也可以处理）。
*/
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
/*
freewalk() 用于递归释放页表结构中所有的中间页表页（非叶子节点），前提是所有叶子页映射（映射到实际物理页的页表项）已经被移除。
*/
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
/*
uvmfree() 先释放用户空间虚拟内存中映射的物理页（通过 uvmunmap()），然后递归释放整个页表结构（通过 freewalk()）。
*/
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
/*
uvmcopy() 将父进程的用户虚拟内存从 old 页表 逐页复制到 new 页表，
包括页表项和实际的物理页面内容，主要用于创建子进程（如 fork()）
*/
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
/*
uvmclear() 将页表中某一页的用户访问权限位 PTE_U 清除，从而禁止用户态访问这页，常用于设置用户栈保护页（guard page）。
*/
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
/*
copyout() 将内核中的数据（src 指向的）拷贝到用户页表 pagetable 对应的虚拟地址 dstva 开始的位置中，
跨页处理，并检查地址合法性。常用于系统调用将内核数据返回给用户进程。
*/
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
/*
copyin() 函数将用户进程虚拟地址空间（srcva）中的数据复制到内核缓冲区 dst 中，
并做合法性检查，支持跨页拷贝。常用于系统调用读取用户输入。
*/
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  // uint64 n, va0, pa0;

  // while(len > 0){
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0);
  //   if(pa0 == 0)
  //     return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if(n > len)
  //     n = len;
  //   memmove(dst, (void *)(pa0 + (srcva - va0)), n);

  //   len -= n;
  //   dst += n;
  //   srcva = va0 + PGSIZE;
  // }
  // return 0;
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
/*
copyinstr() 用于将用户态字符串从虚拟地址 srcva 拷贝到内核缓冲区 dst，直到遇到终止符 \0 或拷贝满 max 字节，
常用于系统调用参数字符串的读取，如 exec("ls")。
*/
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  // uint64 n, va0, pa0;
  // int got_null = 0;

  // while(got_null == 0 && max > 0){
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0);
  //   if(pa0 == 0)
  //     return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if(n > max)
  //     n = max;

  //   char *p = (char *) (pa0 + (srcva - va0)); // 一个字符一个字符地拷贝
  //   while(n > 0){
  //     if(*p == '\0'){
  //       *dst = '\0';
  //       got_null = 1;
  //       break;
  //     } else {
  //       *dst = *p;
  //     }
  //     --n;
  //     --max;
  //     p++;
  //     dst++;
  //   }

  //   srcva = va0 + PGSIZE;
  // }
  // if(got_null){
  //   return 0;
  // } else {
  //   return -1;
  // }
  return copyinstr_new(pagetable, dst, srcva, max);
}

// 递归释放一个内核页表中的所有映射，但是不释放其指向的物理页
void
kama_kvm_free_kernelpgtbl(pagetable_t pagetable) {
    for (int i = 0;i < 512;++i) {
        pte_t pte = pagetable[i];
        uint64 child = PTE2PA(pte);
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {      // 如果该页表项指向更低一级的页表
            kama_kvm_free_kernelpgtbl((pagetable_t)child);                // 递归释放低一级页表及其页表项
            pagetable[i] = 0;
        }
    }
    kfree((void*)pagetable);        // 释放当前级别页表所占用空间
}

// 递归打印页表
int kama_pgtblprint(pagetable_t pagetable, int depth) {
    // there are 2^9 = 512 PTEs in a page table.
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];

        if (pte & PTE_V) {      // 如果页表项有效，按格式打印页表项
            printf("..");
            for (int j = 0; j < depth; ++j)
                printf(" ..");
            printf("%d: pte %p pa %p\n", i, pte, PTE2PA(pte)); // PTE2PA从页表项中提取出对应页帧的物理地址（页首地址）

            // 如果该节点不是叶节点，递归打印子节点
            // 如果一个页表项有效（PTE_V为 1），但 没有 R/W/X 权限，就代表它不是一个映射到物理页的叶子节点，而是中间节点，即指向下一层页表
            if ((pte & (PTE_R | PTE_W | PTE_X)) == 0) {
                // this PTE points to a lower-level page table.
                uint64 child = PTE2PA(pte);
                kama_pgtblprint((pagetable_t)child, depth + 1);
            }
        }
    }
    return 0;
}

// 打印页表
int kama_vmprint(pagetable_t pagetable) {
    printf("page table %p\n", pagetable);
    return kama_pgtblprint(pagetable, 0);
}

// 将 src 页表的一部分页映射关系拷贝到 dst 页表中。只拷贝页表项，不拷贝实际的物理页内存
int
kama_kvmcopymappings(pagetable_t src, pagetable_t dst, uint64 start, uint64 sz) {
    pte_t* pte;
    uint64 pa, i;
    uint flags;

    // PGROUNDUP: 将地址向上取整到页边界，防止重新映射已经映射的页，特别是在执行growproc操作时
    for (i = PGROUNDUP(start);i < start + sz;i += PGSIZE) {
        if ((pte = walk(src, i, 0)) == 0)
            panic("kvmcopymappings: pte should exist");
        if ((*pte & PTE_V) == 0)
            panic("kvmcopymappings: page not present");
        pa = PTE2PA(*pte);

        // `& ~PTE_U` 表示将该页的权限设置为非用户页
        // 必须设置该权限，因为RISC-V 中内核是无法直接访问用户页的
        flags = PTE_FLAGS(*pte) & ~PTE_U;
        if (mappages(dst, i, PGSIZE, pa, flags) != 0)
            goto err;
    }

    return 0;

err:
    //解除目标页表中已映射的页表项
    uvmunmap(dst, PGROUNDUP(start), (i - PGROUNDUP(start)) / PGSIZE, 0);            
    return -1;
}

// 与 uvmdealloc 功能类似，将程序内存从 oldsz 缩减到 newsz，但不释放实际内存
uint64
kama_kvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
    if (newsz >= oldsz)
        return oldsz;

    if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
        int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        uvmunmap(pagetable, PGROUNDUP(newsz), npages, 0);
    }

    return newsz;
}