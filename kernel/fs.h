// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC  文件系统的标识号，用于验证该磁盘是否真的是一个有效的 xv6 文件系统（应为 FSMAGIC）。防止误读。
  uint size;         // Size of file system image (blocks)  整个文件系统镜像的大小（单位是块）。即总共多少个 block，
  uint nblocks;      // Number of data blocks  文件系统中可用的数据块数量，也就是用户文件实际可以使用的空间块数量
  uint ninodes;      // Number of inodes.  文件系统中可用的 inode 数量，即最多能有多少个文件（或目录）。
  uint nlog;         // Number of log blocks  日志区的块数，即用于日志（log）记录的块数。日志用于崩溃恢复和原子性保障。
  uint logstart;     // Block number of first log block  日志区的起始块号（在哪个块开始）。
  uint inodestart;   // Block number of first inode block  inode 区的起始块号。
  uint bmapstart;    // Block number of first free map block  空闲块位图（block bitmap）的起始块号，用来管理哪些块是空闲的。
};

#define FSMAGIC 0x10203040

// #define NDIRECT 12
// #define NINDIRECT (BSIZE / sizeof(uint))
// #define MAXFILE (NDIRECT + NINDIRECT)
#define NDIRECT 11                                               // 直接块数量
#define NINDIRECT (BSIZE / sizeof(uint))                         // 一级间接块数量
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)    // 二级间接块数量

// On-disk inode structure 磁盘上的
// struct dinode {
//   short type;           // File type 零表示磁盘inode是空闲的
//   short major;          // Major device number (T_DEVICE only)
//   short minor;          // Minor device number (T_DEVICE only)
//   short nlink;          // Number of links to inode in file system
//   uint size;            // Size of file (bytes)
//   uint addrs[NDIRECT+1];   // Data block addresses
// };
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+2];   // 0~10：直接索引    11：一级间接索引    12：二级间接索引
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

