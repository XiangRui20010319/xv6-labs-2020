//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
/*
sys_link() 会让一个新的路径 new 指向和已有文件 old 相同的 inode，
从而实现 硬链接：两个路径指向同一个底层数据。
*/
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

/*
create() 创建一个新文件或目录，并将它挂到路径对应的目录中。
它处理 inode 分配、初始化、. / .. 条目创建等底层细节。

path：要创建的路径，比如 /a/b.txt

type：文件类型，T_FILE、T_DIR、T_DEVICE

major/minor：用于设备文件，普通文件无视
*/
static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

/*
sys_open() 根据路径和模式（读/写/创建等）打开一个文件或设备，准备好对应的 struct file 和 inode，然后返回文件描述符 fd。
sys_open(path, omode)
│
├── 获取路径、模式参数
│
├── begin_op() 开始日志事务
│
├── 是否是 O_CREATE？
│   ├── 是 → create() 创建文件
│   └── 否 → namei() 查找已有文件
│
├── 不允许写目录？
├── 非法设备文件？
│
├── filealloc() 分配 struct file
├── fdalloc() 分配文件描述符
│
├── 设置 file 各项字段
├── 是否需要截断内容？
│
├── iunlock()
└── end_op()
    ↓
   返回 fd
*/
// uint64
// sys_open(void)
// {
//   char path[MAXPATH];
//   int fd, omode;
//   struct file *f;
//   struct inode *ip;
//   int n;

//   if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
//     return -1;

//   begin_op();

//   if(omode & O_CREATE){
//     ip = create(path, T_FILE, 0, 0);
//     if(ip == 0){
//       end_op();
//       return -1;
//     }
//   } else {
//     if((ip = namei(path)) == 0){
//       end_op();
//       return -1;
//     }
//     ilock(ip);
//     if(ip->type == T_DIR && omode != O_RDONLY){
//       iunlockput(ip);
//       end_op();
//       return -1;
//     }
//   }

//   if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
//     iunlockput(ip);
//     end_op();
//     return -1;
//   }

//   if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
//     if(f)
//       fileclose(f);
//     iunlockput(ip);
//     end_op();
//     return -1;
//   }

//   if(ip->type == T_DEVICE){
//     f->type = FD_DEVICE;
//     f->major = ip->major;
//   } else {
//     f->type = FD_INODE;
//     f->off = 0;
//   }
//   f->ip = ip;
//   f->readable = !(omode & O_WRONLY);
//   f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

//   if((omode & O_TRUNC) && ip->type == T_FILE){
//     itrunc(ip);
//   }

//   iunlock(ip);
//   end_op();

//   return fd;
// }

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  }
  else {
      int symlink_depth = 0;
      while (1) {
          if ((ip = namei(path)) == 0) {    // 解析路径，获取对应的inode
              end_op();
              return -1;
          }

          ilock(ip);
          if (ip->type == T_SYMLINK && (omode & O_NOFOLLOW) == 0) {     //如果当前指向的仍是软链接，则继续循环
              if (++symlink_depth > 10) {               // 链接深度超过10层就退出
                  iunlockput(ip);
                  end_op();
                  return -1;
              }
              if (readi(ip, 0, (uint64)path, 0, MAXPATH) < 0) {     // 读取链接的目标路径
                  iunlockput(ip);
                  end_op();
                  return -1;
              }
              iunlockput(ip);
          }
          else
              break;
      }

      if (ip->type == T_DIR && omode != O_RDONLY){
          iunlockput(ip);
          end_op();
          return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

/*
创建一个新的目录（文件类型为 T_DIR），路径由用户传入
*/
uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

/*
根据给定的路径、major 和 minor 设备号，在文件系统中创建一个类型为 T_DEVICE 的设备文件。

sys_mknod(path, major, minor)
│
├── begin_op() 开始日志事务
│
├── 获取参数 (path, major, minor)
│
├── 调用 create():
│     └── 创建一个类型为 T_DEVICE 的 inode
│     └── 设置其 major / minor 字段
│     └── 加入目录项
│
├── 解锁并释放 inode（iunlockput）
├── end_op() 提交日志事务
└── 返回 0 表示成功
*/
uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

/*
把当前进程的工作目录（cwd）更改为用户传入路径所指向的目录。
*/
uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

/*
从用户空间读取路径 path 和参数数组 argv；
加载一个新的可执行文件，替换当前进程的内存；
如果成功，进程继续从新程序的 main() 开始运行；
如果失败，则返回 -1。


用户传入 exec("/bin/ls", ["ls", "-l", 0])
↓ sys_exec 读取路径和参数
↓ 依次将用户 argv 拷贝到内核内存
↓ 调用 exec(path, argv)
↓ 成功：不再返回（新程序开始执行）
↓ 失败：释放内存，返回 -1
*/
uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

/*
该函数用于创建一个管道，使一个进程可以通过写端写入数据，另一个进程从读端读取数据，实现进程间通信（IPC）。


用户空间:  int fd[2]; pipe(fd);
↓ sys_pipe 被调用

↓ 创建管道 → 得到 read_file, write_file
↓ 分配 fd0 → 指向 read_file
↓ 分配 fd1 → 指向 write_file
↓ copyout(fd0) → 写入 fd[0]
↓ copyout(fd1) → 写入 fd[1]

返回 0，用户获得 fd[0] 读端, fd[1] 写端
*/
uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// kernel/sysfile.c
// 软链接
uint64
sys_symlink(void) {
    struct inode* ip;
    char target[MAXPATH], path[MAXPATH];
    if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
        return -1;

    begin_op();

    ip = create(path, T_SYMLINK, 0, 0);     // 创建一个新的inode，类型为T_SYMLINK，指向path文件
    if (ip == 0) {
        end_op();
        return -1;
    }

    if (writei(ip, 0, (uint64)target, 0, strlen(target)) < 0) {     // 将target路径写入inode
        end_op();
        return -1;
    }

    iunlockput(ip);
    end_op();

    return 0;
}