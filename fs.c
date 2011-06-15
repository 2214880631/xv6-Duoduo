// File system implementation.  Four layers:
//   + Blocks: allocator for raw disk blocks.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// Disk layout is: superblock, inodes, block in-use bitmap, data blocks.
//
// This file contains the low-level file system manipulation 
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "spinlock.h"
#include "condvar.h"
#include "queue.h"
#include "proc.h"
#include "buf.h"
#include "fs.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
static void itrunc(struct inode*);

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;
  
  bp = bread(dev, 1, 0);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp, 0);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;
  
  bp = bread(dev, bno, 1);
  memset(bp->data, 0, BSIZE);
  bwrite(bp);
  brelse(bp, 1);
}

// Blocks. 

// Allocate a disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;
  struct superblock sb;

  bp = 0;
  readsb(dev, &sb);
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb.ninodes), 1);
    for(bi = 0; bi < BPB; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use on disk.
        bwrite(bp);
        brelse(bp, 1);
        return b + bi;
      }
    }
    brelse(bp, 1);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  struct superblock sb;
  int bi, m;

  bzero(dev, b);

  readsb(dev, &sb);
  bp = bread(dev, BBLOCK(b, sb.ninodes), 1);
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;  // Mark block free on disk.
  bwrite(bp);
  brelse(bp, 1);
}

// Inodes.
//
// An inode is a single, unnamed file in the file system.
// The inode disk structure holds metadata (the type, device numbers,
// and data size) along with a list of blocks where the associated
// data can be found.
//
// The inodes are laid out sequentially on disk immediately after
// the superblock.  The kernel keeps a cache of the in-use
// on-disk structures to provide a place for synchronizing access
// to inodes shared between multiple processes.
// 
// ip->ref counts the number of pointer references to this cached
// inode; references are typically kept in struct file and in proc->cwd.
// When ip->ref falls to zero, the inode is no longer cached.
// It is an error to use an inode without holding a reference to it.
//
// Processes are only allowed to read and write inode
// metadata and contents when holding the inode's lock,
// represented by the I_BUSY flag in the in-memory copy.
// Because inode locks are held during disk accesses, 
// they are implemented using a flag rather than with
// spin locks.  Callers are responsible for locking
// inodes before passing them to routines in this file; leaving
// this responsibility with the caller makes it possible for them
// to create arbitrarily-sized atomic operations.
//
// To give maximum control over locking to the callers, 
// the routines in this file that return inode pointers 
// return pointers to *unlocked* inodes.  It is the callers'
// responsibility to lock them before using them.  A non-zero
// ip->ref keeps these unlocked inodes in the cache.

static struct ns *ins;

void
iinit(void)
{
  ins = nsalloc(0);
  for (int i = 0; i < NINODE; i++) {
    struct inode *ip = kmalloc(sizeof(*ip));
    memset(ip, 0, sizeof(*ip));
    ip->inum = -i-1;
    initlock(&ip->lock, "icache-lock");
    initcondvar(&ip->cv, "icache-cv");
    ns_insert(ins, ip->inum, ip);
  }
}

//PAGEBREAK!
// Allocate a new inode with the given type on device dev.
// Returns a locked inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;
  struct superblock sb;

  readsb(dev, &sb);
  for(inum = 1; inum < sb.ninodes; inum++){  // loop over inode blocks
    bp = bread(dev, IBLOCK(inum), 0);
    dip = (struct dinode*)bp->data + inum%IPB;
    int seemsfree = (dip->type == 0);
    brelse(bp, 0);
    if(seemsfree){
      // maybe this inode is free. look at it via the
      // inode cache to make sure.
      struct inode *ip = iget(dev, inum);
      ilock(ip, 1);
      if(ip->type == 0){
        ip->type = type;
        ip->gen += 1;
        if(ip->nlink || ip->size || ip->addrs[0])
          panic("ialloc not zeroed");
        iupdate(ip);
        return ip;
      }
      iunlockput(ip);
      cprintf("ialloc oops %d\n", inum); // XXX harmless
    }
  }
  panic("ialloc: no inodes");
}

// Copy inode, which has changed, from memory to disk.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum), 1);
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  dip->gen = ip->gen;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  bwrite(bp);
  brelse(bp, 1);
}

static void *
evict(uint key, void *p)
{
  struct inode *ip = p;
  acquire(&ip->lock);
  if (ip->ref == 0)
    return ip;
  release(&ip->lock);
  return 0;
}

// Find the inode with number inum on device dev
// and return the in-memory copy.
// The inode is not locked, so someone else might
// be modifying it.
// But it has a ref count, so it won't be freed or reused.
// Though unlocked, all fields will be present,
// so looking a ip->inum and ip->gen are OK even w/o lock.
struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip;

 retry:
  // Try for cached inode.
  rcu_begin_read();
  ip = ns_lookup(ins, inum);	// XXX ignore dev
  if (ip) {
    if (ip->dev != dev) panic("iget dev mismatch");
    // tricky: first bump ref, then check free flag
    __sync_fetch_and_add(&ip->ref, 1);
    if (ip->flags & I_FREE) {
      rcu_end_read();
      __sync_sub_and_fetch(&ip->ref, 1);
      goto retry;
    }
    rcu_end_read();
    if (!(ip->flags & I_VALID)) {
      acquire(&ip->lock);
      while((ip->flags & I_VALID) == 0)
	cv_sleep(&ip->cv, &ip->lock);
      release(&ip->lock);
    }
    return ip;
  }
  rcu_end_read();

  // Allocate fresh inode cache slot.
 retry_evict:
  (void) 0;
  struct inode *victim = ns_enumerate(ins, evict);
  if (!victim)
    panic("iget out of space");
  // tricky: first flag as free, then check refcnt, then remove from ns
  victim->flags |= I_FREE;
  if (victim->ref > 0) {
    victim->flags &= ~(I_FREE);
    release(&victim->lock);
    goto retry_evict;
  }
  release(&victim->lock);
  ns_remove(ins, victim->inum, victim);
  rcu_delayed(victim, kmfree);
  
  ip = kmalloc(sizeof(*ip));
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->flags = I_BUSYR | I_BUSYW;
  ip->readbusy = 1;
  snprintf(ip->lockname, sizeof(ip->lockname), "cv:ino:%d", ip->inum);
  initlock(&ip->lock, ip->lockname+3);
  initcondvar(&ip->cv, ip->lockname);
  if (ns_insert(ins, ip->inum, ip) < 0) {
    rcu_delayed(ip, kmfree);
    goto retry;
  }
  
  struct buf *bp = bread(ip->dev, IBLOCK(ip->inum), 0);
  struct dinode *dip = (struct dinode*)bp->data + ip->inum%IPB;
  ip->type = dip->type;
  ip->major = dip->major;
  ip->minor = dip->minor;
  ip->nlink = dip->nlink;
  ip->size = dip->size;
  ip->gen = dip->gen;
  memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
  brelse(bp, 0);
  ip->flags |= I_VALID;

  iunlock(ip);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  __sync_fetch_and_add(&ip->ref, 1);
  return ip;
}

// Lock the given inode.
// XXX why does ilock() read the inode from disk?
// why doesn't the iget() that allocated the inode cache entry
// read the inode from disk?
void
ilock(struct inode *ip, int writer)
{
  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquire(&ip->lock);
  while(ip->flags & (I_BUSYW | (writer ? I_BUSYR : 0)))
    cv_sleep(&ip->cv, &ip->lock);
  ip->flags |= I_BUSYR | (writer ? I_BUSYW : 0);
  __sync_fetch_and_add(&ip->readbusy, 1);
  release(&ip->lock);

  if((ip->flags & I_VALID) == 0)
    panic("ilock");
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !(ip->flags & (I_BUSYR | I_BUSYW)) || ip->ref < 1)
    panic("iunlock");

  acquire(&ip->lock);
  int lastreader = __sync_sub_and_fetch(&ip->readbusy, 1);
  ip->flags &= ~(I_BUSYW | ((lastreader==0) ? I_BUSYR : 0));
  cv_wakeup(&ip->cv);
  release(&ip->lock);
}

// Caller holds reference to unlocked ip.  Drop reference.
void
iput(struct inode *ip)
{
  if(__sync_sub_and_fetch(&ip->ref, 1) == 0) {
    acquire(&ip->lock);
    if (ip->ref == 0 && ip->nlink == 0) {
      // inode is no longer used: truncate and free inode.
      if(ip->flags & (I_BUSYR | I_BUSYW))
	panic("iput busy");
      if((ip->flags & I_VALID) == 0)
	panic("iput not valid");
      ip->flags |= I_BUSYR | I_BUSYW;
      __sync_fetch_and_add(&ip->readbusy, 1);
      release(&ip->lock);
      itrunc(ip);
      ip->type = 0;
      ip->major = 0;
      ip->minor = 0;
      ip->gen += 1;
      iupdate(ip);
      acquire(&ip->lock);
      ip->flags &= ~(I_BUSYR | I_BUSYW);
      __sync_sub_and_fetch(&ip->readbusy, 1);
      cv_wakeup(&ip->cv);
    }
    release(&ip->lock);
  }
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode contents
//
// The contents (data) associated with each inode is stored
// in a sequence of blocks on the disk.  The first NDIRECT blocks
// are listed in ip->addrs[].  The next NINDIRECT blocks are 
// listed in the block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr, 1);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      bwrite(bp);
    }
    brelse(bp, 1);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Only called after the last dirent referring
// to this inode has been erased on disk.
static void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      rcu_delayed2(ip->dev, ip->addrs[i], bfree);
      ip->addrs[i] = 0;
    }
  }
  
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT], 0);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        rcu_delayed2(ip->dev, a[j], bfree);
    }
    brelse(bp, 0);
    rcu_delayed2(ip->dev, ip->addrs[NDIRECT], bfree);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE), 0);
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp, 0);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    n = MAXFILE*BSIZE - off;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE), 1);
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(bp->data + off%BSIZE, src, m);
    bwrite(bp);
    brelse(bp, 1);
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
// Caller must have already locked dp.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct buf *bp;
  struct dirent *de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += BSIZE){
    bp = bread(dp->dev, bmap(dp, off / BSIZE), 0);
    for(de = (struct dirent*)bp->data;
        de < (struct dirent*)(bp->data + BSIZE);
        de++){
      if(de->inum == 0)
        continue;
      if(namecmp(name, de->name) == 0){
        // entry matches path element
        if(poff)
          *poff = off + (uchar*)de - bp->data;
        inum = de->inum;
        brelse(bp, 0);
        return iget(dp->dev, inum);
      }
    }
    brelse(bp, 0);
  }
  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");
  
  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/') 
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(proc->cwd);

  while((path = skipelem(path, name)) != 0){
    next = 0;
    if(nameiparent == 0)
      next = nc_lookup(ip, name);
    if(next == 0){
      ilock(ip, 0);
      if(ip->type == 0)
        panic("namex");
      if(ip->type != T_DIR){
        iunlockput(ip);
        return 0;
      }
      if(nameiparent && *path == '\0'){
        // Stop one level early.
        iunlock(ip);
        return ip;
      }
      if((next = dirlookup(ip, name, 0)) == 0){
        iunlockput(ip);
        return 0;
      }
      nc_insert(ip, name, next);
      iunlockput(ip);
    }
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
