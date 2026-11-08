/*
 * Author(s): <Your name here>
 * COS 318, Fall 2019: Project 6 File System.
 * Implementation of a Unix-like file system.
*/
#include "util.h"
#include "common.h"
#include "block.h"
#include "fs.h"

#ifdef FAKE
#include <stdio.h>
#define ERROR_MSG(m) printf m;
#else
#include "thread.h"
#define ERROR_MSG(m)
#endif
/* ------------------------------------------------------------------ */
/* On-disk layout (FS_SIZE = 2048 blocks of 512 bytes)                 */
/*                                                                     */
/*   block 0                     : superblock (magic word)             */
/*   block 1                     : inode bitmap (FS_SIZE bits)         */
/*   block 2                     : data block bitmap (FS_SIZE bits)    */
/*   block 3 .. 3+INODE_TBL-1    : inode table, 11 inodes per block    */
/*   block 190 .. 2047           : data blocks                         */
/*                                                                     */
/* Each file/directory has one inode with 8 direct block pointers, so   */
/* the maximum file size is 8 * 512 = 4096 bytes.                       */
/* ------------------------------------------------------------------ */

#define FS_MAGIC          0x318f3186

#define MAX_INODES        FS_SIZE
#define INODES_PER_BLOCK  (BLOCK_SIZE / (int) sizeof(inode_t))  /* 11 */
#define INODE_TABLE_START 3
#define INODE_TABLE_BLOCKS 187            /* ceil(MAX_INODES/11) */
#define DATA_START        (INODE_TABLE_START + INODE_TABLE_BLOCKS)
#define MAX_FILE_SIZE     (8 * BLOCK_SIZE)
#define MAX_OPEN_FILES    256             /* handles 0..255 */
#define ROOT_INODE        0
#define WALK_DEPTH        128

/* 44 bytes */
typedef struct {
  int type;                /* FREE_INODE, DIRECTORY or FILE_TYPE */
  int links;               /* hard link count */
  int size;                /* size in bytes */
  int dir[8];              /* direct block pointers, 0 = none */
} inode_t;

/* 36 bytes; a directory is stored as a flat, packed array of these */
typedef struct {
  int inode;               /* inode number, 0 = unused */
  char name[MAX_FILE_NAME];
} dirent_t;

/* One entry per open file handle */
typedef struct {
  int in_use;
  int inode;
  int offset;              /* per-descriptor offset into the file */
  int mode;                /* FS_O_RDONLY / FS_O_WRONLY / FS_O_RDWR */
} fd_entry_t;

/* The current working directory, an absolute path */
static char cwd[MAX_PATH_NAME] = "/";

static fd_entry_t fds[MAX_OPEN_FILES];

static int fs_ready = FALSE;

#ifndef FAKE
/* Protects the file system meta data against concurrent syscalls */
static lock_t fs_lock;
static int fs_lock_inited = FALSE;
static void fs_enter(void)
{
  if (!fs_lock_inited) {
    lock_init(&fs_lock);
    fs_lock_inited = TRUE;
  }
  lock_acquire(&fs_lock);
}
static void fs_leave(void)
{
  lock_release(&fs_lock);
}
#else
static void fs_enter(void) {}
static void fs_leave(void) {}
#endif

/* One-block cache: block access through the USB stack is expensive,
 * so consecutive accesses to the same block are cached */
static int cache_block_no = -1;
static char cache_block_data[BLOCK_SIZE];

static void cache_block_read(int block, char *dest)
{
  if (cache_block_no == block) {
    bcopy(cache_block_data, dest, BLOCK_SIZE);
    return;
  }
  block_read(block, dest);
  cache_block_no = block;
  bcopy(dest, cache_block_data, BLOCK_SIZE);
}

static void cache_block_write(int block, char *src)
{
  block_write(block, src);
  cache_block_no = block;
  bcopy(src, cache_block_data, BLOCK_SIZE);
}

/* ------------------------------------------------------------------ */
/* Bitmaps                                                             */
/* ------------------------------------------------------------------ */

static int bit_test(char *bm, int i)
{
  return (bm[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(char *bm, int i)
{
  bm[i >> 3] |= (char) (1 << (i & 7));
}

static void bit_clear(char *bm, int i)
{
  bm[i >> 3] &= (char) ~(1 << (i & 7));
}

/* Mark the first zero bit in [0,limit) and return its index, or -1 */
static int bitmap_alloc(char *bm, int limit)
{
  int i;

  for (i = 0; i < limit; ++i) {
    if (!bit_test(bm, i)) {
      bit_set(bm, i);
      return i;
    }
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* Inodes                                                              */
/* ------------------------------------------------------------------ */

static void read_inode(int ino, inode_t *node)
{
  char buf[BLOCK_SIZE];
  int block = INODE_TABLE_START + ino / INODES_PER_BLOCK;

  cache_block_read(block, buf);
  bcopy(buf + (ino % INODES_PER_BLOCK) * (int) sizeof(inode_t),
        (char *) node, sizeof(inode_t));
}

static void write_inode(int ino, inode_t *node)
{
  char buf[BLOCK_SIZE];
  int block = INODE_TABLE_START + ino / INODES_PER_BLOCK;

  cache_block_read(block, buf);
  bcopy((char *) node,
        buf + (ino % INODES_PER_BLOCK) * (int) sizeof(inode_t),
        sizeof(inode_t));
  cache_block_write(block, buf);
}

static int inode_alloc(void)
{
  char bm[BLOCK_SIZE];
  int ino;

  cache_block_read(1, bm);
  ino = bitmap_alloc(bm, MAX_INODES);
  if (ino < 0)
    return -1;
  cache_block_write(1, bm);
  return ino;
}

static void inode_free(int ino)
{
  char bm[BLOCK_SIZE];

  cache_block_read(1, bm);
  bit_clear(bm, ino);
  cache_block_write(1, bm);
}

/* Allocate a zeroed data block; returns its ABSOLUTE block number
 * (>= DATA_START) or -1 when the disk is full */
static int data_alloc(void)
{
  char bm[BLOCK_SIZE];
  int rel, block;

  cache_block_read(2, bm);
  rel = bitmap_alloc(bm, FS_SIZE - DATA_START);
  if (rel < 0)
    return -1;
  cache_block_write(2, bm);

  /* Zero the contents of the new data block */
  block = DATA_START + rel;
  bzero(cache_block_data, BLOCK_SIZE);
  cache_block_write(block, cache_block_data);

  return block;
}

static void data_free(int block)
{
  char bm[BLOCK_SIZE];

  if (block < DATA_START)
    return;
  cache_block_read(2, bm);
  bit_clear(bm, block - DATA_START);
  cache_block_write(2, bm);
}

/* Count the data blocks used by an inode */
static int count_blocks(inode_t *node)
{
  int i, n = 0;

  for (i = 0; i < 8; ++i)
    if (node->dir[i] != 0)
      ++n;
  return n;
}

/* Free all data blocks referenced by an inode */
static void free_blocks(inode_t *node)
{
  int i;

  for (i = 0; i < 8; ++i) {
    if (node->dir[i] != 0) {
      data_free(node->dir[i]);
      node->dir[i] = 0;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Data block access                                                   */
/* ------------------------------------------------------------------ */

/* Make sure the blocks that hold [off, off+len) exist in the inode.
 * Allocates zeroed data blocks as needed; rolls back on failure.
 * Returns 0 on success, -1 when a new block cannot be allocated or
 * the range extends past the end of the inode's block list. */
static int inode_ensure_blocks(inode_t *node, int off, int len)
{
  int first, last, i, j, claimed[8], nclaimed = 0;

  if (len <= 0)
    return 0;
  first = off / BLOCK_SIZE;
  last = (off + len - 1) / BLOCK_SIZE;
  if (off < 0 || last >= 8)
    return -1;

  for (i = first; i <= last; ++i) {
    if (node->dir[i] == 0) {
      int block = data_alloc();

      if (block < 0) {
        /* Roll back the blocks we just allocated */
        for (j = 0; j < nclaimed; ++j) {
          data_free(node->dir[claimed[j]]);
          node->dir[claimed[j]] = 0;
        }
        return -1;
      }
      node->dir[i] = block;
      claimed[nclaimed++] = i;
    }
  }
  return 0;
}

/* Copy out 'len' bytes starting at file offset 'off'. Blocks that are
 * not allocated read back as zeros (sparse regions). */
static void inode_read_data(inode_t *node, int off, char *dest, int len)
{
  int done = 0;

  while (done < len) {
    int bi = (off + done) / BLOCK_SIZE;
    int bo = (off + done) % BLOCK_SIZE;
    int n = BLOCK_SIZE - bo;
    char buf[BLOCK_SIZE];
    int block;

    if (n > len - done)
      n = len - done;

    block = (bi < 8) ? node->dir[bi] : 0;
    if (block == 0) {
      /* Absent block: reads back as zeros */
      bzero(dest + done, n);
    } else {
      cache_block_read(block, buf);
      bcopy(buf + bo, dest + done, n);
    }
    done += n;
  }
}

/* Write 'len' bytes at file offset 'off'. All needed blocks are
 * allocated first, so the write either succeeds completely or fails
 * without corrupting the file. Returns 0 or -1. */
static int inode_write_data(inode_t *node, int off, char *src, int len)
{
  int done = 0;

  if (len <= 0)
    return 0;
  if (inode_ensure_blocks(node, off, len) < 0)
    return -1;

  while (done < len) {
    int bi = (off + done) / BLOCK_SIZE;
    int bo = (off + done) % BLOCK_SIZE;
    int n = BLOCK_SIZE - bo;
    char buf[BLOCK_SIZE];

    if (n > len - done)
      n = len - done;

    cache_block_read(node->dir[bi], buf);
    bcopy(src + done, buf + bo, n);
    cache_block_write(node->dir[bi], buf);

    done += n;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Names and directories                                               */
/* ------------------------------------------------------------------ */

/* Bounded name copy, always NUL-terminated. Returns -1 if 'src' is
 * too long, 0 otherwise. */
static int copy_name(char *dst, char *src)
{
  int i;

  for (i = 0; i < MAX_FILE_NAME - 1 && src[i] != '\0'; ++i)
    dst[i] = src[i];
  if (src[i] != '\0')
    return -1;              /* truncated: name too long */
  dst[i] = '\0';
  return 0;
}

static int name_ok(char *s)
{
  int len = strlen(s);

  return (len > 0 && len < MAX_FILE_NAME);
}

/* TRUE if 'dir' is 'prefix' itself or lies below it; both are
 * absolute paths where "/" denotes the root */
static int path_inside(char *prefix, char *dir)
{
  int i, plen = strlen(prefix);

  if (strlen(prefix) == 1 && prefix[0] == '/')
    return TRUE;                 /* everything is inside the root */
  for (i = 0; i < plen; ++i)
    if (dir[i] != prefix[i])
      return FALSE;
  return dir[i] == '\0' || dir[i] == '/';
}

/* An entry is valid if it names a live file or directory */
static int is_dot(char *s)
{
  return s[0] == '.' && (s[1] == '\0' ||
       (s[1] == '.' && s[2] == '\0'));
}

/* Find 'name' in the directory 'dirno'. Returns the inode number of
 * the entry, or -1 when it does not exist. */
static int dir_find_entry(int dirno, char *name)
{
  inode_t node;
  dirent_t entry;
  int i, n;

  if (dirno < 0)
    return -1;
  read_inode(dirno, &node);
  n = node.size / (int) sizeof(dirent_t);

  for (i = 0; i < n; ++i) {
    inode_read_data(&node, i * (int) sizeof(dirent_t),
        (char *) &entry, sizeof(dirent_t));
    if (entry.inode > 0 && same_string(entry.name, name))
      return entry.inode;
  }
  return -1;
}

/* Append the entry (name, ino) to directory 'dirno' */
static int dir_add_entry(int dirno, char *name, int ino)
{
  inode_t node;
  dirent_t entry;
  int i, n;

  read_inode(dirno, &node);
  n = node.size / (int) sizeof(dirent_t);

  /* The name must not already exist, and must fit */
  if (n * (int) sizeof(dirent_t) + (int) sizeof(dirent_t)
      > MAX_FILE_SIZE)
    return -1;              /* directory full (no block left) */
  if (copy_name(entry.name, name) < 0)
    return -1;              /* name too long */
  for (i = 0; i < n; ++i) {
    dirent_t tmp;
    inode_read_data(&node, i * (int) sizeof(dirent_t),
        (char *) &tmp, sizeof(dirent_t));
    if (tmp.inode > 0 && same_string(tmp.name, name))
      return -1;            /* entry already exists */
  }

  entry.inode = ino;
  if (inode_write_data(&node, node.size, (char *) &entry,
          sizeof(dirent_t)) < 0)
    return -1;

  node.size += sizeof(dirent_t);
  write_inode(dirno, &node);
  return 0;
}

/* Remove the entry 'name' from directory 'dirno' and compact it by
 * moving the final entry into the freed slot */
static int dir_remove_entry(int dirno, char *name)
{
  inode_t node;
  dirent_t entry;
  int i, n, idx = -1;

  read_inode(dirno, &node);
  n = node.size / (int) sizeof(dirent_t);

  for (i = 0; i < n; ++i) {
    inode_read_data(&node, i * (int) sizeof(dirent_t),
        (char *) &entry, sizeof(dirent_t));
    if (entry.inode > 0 && same_string(entry.name, name)) {
      idx = i;
      break;
    }
  }
  if (idx < 0)
    return -1;

  /* Move the last entry into the hole (unless it IS the hole) */
  if (idx != n - 1) {
    inode_read_data(&node, (n - 1) * (int) sizeof(dirent_t),
        (char *) &entry, sizeof(dirent_t));
    inode_write_data(&node, idx * (int) sizeof(dirent_t),
        (char *) &entry, sizeof(dirent_t));
  }

  /* Shrink the directory by one entry */
  node.size -= sizeof(dirent_t);
  write_inode(dirno, &node);

  /* Free a directory data block that is no longer needed */
  for (i = 0; i < 8; ++i) {
    int start = i * BLOCK_SIZE;

    if (node.dir[i] != 0 && node.size <= start) {
      data_free(node.dir[i]);
      node.dir[i] = 0;
    }
  }
  write_inode(dirno, &node);
  return 0;
}

/* Enumerate entry #idx of directory 'dirno' */
static int dir_get_entry(int dirno, int idx, dirent_t *entry)
{
  inode_t node;

  if (dirno < 0)
    return -1;
  read_inode(dirno, &node);
  if (idx < 0 || idx * (int) sizeof(dirent_t) >= node.size)
    return -1;
  inode_read_data(&node, idx * (int) sizeof(dirent_t),
      (char *) entry, sizeof(dirent_t));
  return 0;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/*                                                                     */
/* All lookups walk from the root, remembering the chain of            */
/* directories entered on the way; this makes both '.' and '..' easy   */
/* without any on-disk parent pointers.                                */
/* ------------------------------------------------------------------ */

/* Resolve 'path' (relative to the cwd, or absolute) and return:
 *   - the inode containing the final component in '*parent' (or the
 *     component's own inode if the path ends in a directory),
 *   - the inode of the final component in '*target' (-1 if absent),
 *   - its name in 'name' (empty for '.'/'..'/directory paths),
 *   - the absolute path of the final component in 'abs' (an empty
 *     string stands for "/"). */
static int resolve_path(char *path, int *parent, int *target,
    char *name, char *abs)
{
  char full[MAX_PATH_NAME * 2];
  char *comps[WALK_DEPTH];
  char walk[MAX_PATH_NAME];    /* absolute path of the current chain top */
  int chain[WALK_DEPTH];
  int depth = 0;
  int ncomps = 0, i, child;
  char *s, *d, *e;
  inode_t node;

  /* Build the absolute path */
  d = full;
  if (path[0] == '/') {
    for (s = path; *s != '\0' && d < full + MAX_PATH_NAME * 2 - 1; ++s)
      *d++ = *s;
  } else {
    if (cwd[1] != '\0') {        /* cwd is not the root "/" */
      for (s = cwd; *s != '\0' && d < full + MAX_PATH_NAME * 2 - 1; ++s)
        *d++ = *s;
      if (d < full + MAX_PATH_NAME * 2 - 1)
        *d++ = '/';
      else {
        return -1;
      }
    }
    for (s = path; *s != '\0' && d < full + MAX_PATH_NAME * 2 - 1; ++s)
      *d++ = *s;
  }
  *d = '\0';

  /* Split into components */
  s = full;
  while (*s != '\0') {
    while (*s == '/')
      ++s;
    if (*s == '\0')
      break;
    e = s;
    while (*e != '\0' && *e != '/')
      ++e;
    if (ncomps >= WALK_DEPTH) {
      return -1;
    }
    comps[ncomps++] = s;
    if (*e != '\0')
      *e++ = '\0';
    s = e;
  }

  /* Walk from the root */
  depth = 1;
  chain[0] = ROOT_INODE;
  walk[0] = '\0';                /* "" stands for the root "/" */

  for (i = 0; i < ncomps; ++i) {
    char *cp = comps[i];

    if (is_dot(cp)) {
      if (cp[1] == '.' ) {
        /* '..': step back up the chain */
        if (depth > 1) {
          --depth;
          child = strlen(walk);
          while (child > 0 && walk[child] != '/')
            --child;
          if (child > 0)
            walk[child] = '\0';   /* keep the leading part intact */
          else
            walk[0] = '\0';
        }
      }
      /* '.' does not change anything */
      if (i == ncomps - 1) {
        /* The path ends in '.'/'..': the target is the dir itself */
        if (parent != 0) *parent = (depth > 1) ? -2 : chain[0];
        if (target != 0) *target = chain[depth - 1];
        if (name != 0) name[0] = '\0';
        /* build abs: prepend a leading '/' unless root */
        if (walk[0] == '\0') {
          if (abs != 0) { abs[0] = '/'; abs[1] = '\0'; }
        } else {
          if (abs != 0) {
            abs[0] = '\0';
            for (child = 0; walk[child] != '\0'; ++child)
              abs[child] = walk[child];
            abs[child] = '\0';
          }
        }
        return 0;
      }
      continue;
    }

    /* A regular component */
    child = dir_find_entry(chain[depth - 1], cp);
    if (i == ncomps - 1) {
      /* Final component */
      if (parent != 0) *parent = chain[depth - 1];
      if (target != 0) *target = child;
      if (name != 0 && copy_name(name, cp) < 0) {
        return -1;
      }
      if (abs != 0) {
        int w = 0, a = 0;

        if (walk[0] == '\0') {
          abs[a++] = '/';
        } else {
          while (walk[w] != '\0')
            abs[a++] = walk[w++];
          abs[a++] = '/';
        }
        for (w = 0; cp[w] != '\0' && a < MAX_PATH_NAME - 1; ++w)
          abs[a++] = cp[w];
        abs[a] = '\0';
      }
      return 0;
    }
    if (child < 0) {
      return -1;                 /* intermediate does not exist */
    }
    read_inode(child, &node);
    if (node.type != DIRECTORY) {
      return -1;                 /* not a directory */
    }
    if (depth >= WALK_DEPTH) {
      return -1;
    }
    chain[depth++] = child;
    /* Update walk: append "/<cp>" */
    {
      int w = 0, a = 0;
      char tmp[MAX_PATH_NAME];

      if (walk[0] != '\0') {
        while (walk[w] != '\0')
          tmp[a++] = walk[w++];
      }
      tmp[a++] = '/';
      for (w = 0; cp[w] != '\0'; ++w)
        tmp[a++] = cp[w];
      tmp[a] = '\0';
      /* the intermediate path may not exceed the cwd buffer */
      if (a >= MAX_PATH_NAME) {
        return -1;
      }
      for (w = 0; w < a; ++w)
        walk[w] = tmp[w];
      walk[a] = '\0';
    }
  }

  /* The path is empty or ends with '/' (target is a directory) */
  if (depth > 1) {
    /* parent is unknown without an on-disk '..'; use the final dir's
     * own inode: the caller must not create entries in this case */
    if (parent != 0) *parent = -2;
  } else {
    if (parent != 0) *parent = chain[0];       /* the root itself */
  }
  if (target != 0) *target = chain[depth - 1];
  if (name != 0) name[0] = '\0';
  if (abs != 0) {
    if (walk[0] == '\0') {
      abs[0] = '/';
      abs[1] = '\0';
    } else {
      for (i = 0; walk[i] != '\0'; ++i)
        abs[i] = walk[i];
      abs[i] = '\0';
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* File descriptor table                                               */
/* ------------------------------------------------------------------ */

static int fd_alloc(int ino, int mode)
{
  int i;

  for (i = 0; i < MAX_OPEN_FILES; ++i) {
    if (!fds[i].in_use) {
      fds[i].in_use = TRUE;
      fds[i].inode = ino;
      fds[i].offset = 0;
      fds[i].mode = mode;
      return i;
    }
  }
  return -1;                     /* out of file handles */
}

static int fd_check(int fd)
{
  return (fd >= 0 && fd < MAX_OPEN_FILES && fds[fd].in_use);
}

/* ------------------------------------------------------------------ */
/* The file system API                                                 */
/* ------------------------------------------------------------------ */

void fs_init(void)
{
  char buf[BLOCK_SIZE];
  int i;

  block_init();
  cache_block_no = -1;
  fs_ready = FALSE;

  /* Close every file handle and reset the cwd */
  for (i = 0; i < MAX_OPEN_FILES; ++i)
    fds[i].in_use = FALSE;
  for (i = 0; i < MAX_PATH_NAME; ++i)
    cwd[i] = '\0';
  cwd[0] = '/';

  /* A valid superblock marks a mounted file system */
  cache_block_read(0, buf);
  if (*(int *) buf == FS_MAGIC) {
    fs_ready = TRUE;
    ERROR_MSG(("fs_init: file system mounted\n"));
  } else {
    ERROR_MSG(("fs_init: no file system (run mkfs)\n"));
  }
}

int fs_mkfs(void)
{
  static char zero[BLOCK_SIZE];
  int i, block;
  inode_t root;

  fs_enter();

  if (!fs_ready) {
    /* burn in the superblock first so that errors can be reported */
  }

  /* Superblock */
  bzero(zero, BLOCK_SIZE);
  *(int *) zero = FS_MAGIC;
  cache_block_write(0, zero);
  fs_ready = TRUE;

  /* Clear both bitmaps */
  cache_block_write(1, zero);
  cache_block_write(2, zero);
  cache_block_no = -1;           /* force re-read of the bitmaps */

  /* Wipe the inode table */
  for (i = 0; i < INODE_TABLE_BLOCKS; ++i)
    cache_block_write(INODE_TABLE_START + i, zero);

  /* Bring up the root directory: one inode and one data block */
  block = data_alloc();
  if (block < 0) {
    fs_ready = FALSE;
    fs_leave();
    return -1;
  }

  root.type = DIRECTORY;
  root.links = 1;
  root.size = 0;
  for (i = 0; i < 8; ++i)
    root.dir[i] = 0;
  root.dir[0] = block;
  write_inode(ROOT_INODE, &root);

  /* Mark the root directory's inode as used */
  {
    char bm[BLOCK_SIZE];
    cache_block_read(1, bm);
    bit_set(bm, ROOT_INODE);
    cache_block_write(1, bm);
  }

  /* Reset the cwd and file handles */
  for (i = 0; i < MAX_OPEN_FILES; ++i)
    fds[i].in_use = FALSE;
  for (i = 0; i < MAX_PATH_NAME; ++i)
    cwd[i] = '\0';
  cwd[0] = '/';

  fs_leave();
  return 0;
}

int fs_open(char *fileName, int flags)
{
  int parent, target, ino, fd, i;
  char name[MAX_FILE_NAME], abs[MAX_PATH_NAME];
  inode_t node;

  fs_enter();

  if (!fs_ready) {
    fs_leave();
    return -1;
  }
  if (flags != FS_O_RDONLY && flags != FS_O_WRONLY && flags != FS_O_RDWR) {
    fs_leave();
    return -1;
  }
  if (resolve_path(fileName, &parent, &target, name, abs) < 0) {
    fs_leave();
    return -1;
  }

  if (target >= 0) {
    /* Existing entry */
    read_inode(target, &node);
    if (node.type == DIRECTORY && flags != FS_O_RDONLY) {
      /* directories can only be opened for reading */
      fs_leave();
      return -1;
    }
    ino = target;
  } else {
    /* The file does not exist yet: create it */
    if (parent < 0 || name[0] == '\0') {
      fs_leave();
      return -1;
    }
    ino = inode_alloc();
    if (ino < 0) {
      fs_leave();
      return -1;                 /* out of inodes */
    }
    node.type = FILE_TYPE;
    node.links = 1;
    node.size = 0;
    for (i = 0; i < 8; ++i)
      node.dir[i] = 0;
    write_inode(ino, &node);
    if (dir_add_entry(parent, name, ino) < 0) {
      inode_free(ino);
      fs_leave();
      return -1;                 /* duplicate/full directory, etc. */
    }
  }

  fd = fd_alloc(ino, flags);
  if (fd < 0) {
    fs_leave();
    return -1;                   /* out of file handles */
  }

  fs_leave();
  return fd;
}

int fs_close(int fd)
{
  fs_enter();

  if (!fs_ready || !fd_check(fd)) {
    fs_leave();
    return -1;
  }
  fds[fd].in_use = FALSE;

  fs_leave();
  return 0;
}

int fs_read(int fd, char *buf, int count)
{
  inode_t node;
  int offset, available, n;

  fs_enter();

  if (!fs_ready || !fd_check(fd) || count < 0 ||
      fds[fd].mode == FS_O_WRONLY) {
    fs_leave();
    return -1;
  }

  read_inode(fds[fd].inode, &node);
  offset = fds[fd].offset;
  available = node.size - offset;
  n = (count < available) ? count : available;
  if (n < 0)
    n = 0;

  inode_read_data(&node, offset, buf, n);
  fds[fd].offset += n;

  fs_leave();
  return n;
}

int fs_write(int fd, char *buf, int count)
{
  inode_t node;
  int offset, n = count;

  fs_enter();

  if (!fs_ready || !fd_check(fd) || count < 0 ||
      fds[fd].mode == FS_O_RDONLY) {
    fs_leave();
    return -1;
  }

  read_inode(fds[fd].inode, &node);
  offset = fds[fd].offset;
  if (offset > MAX_FILE_SIZE) {
    fs_leave();
    return -1;
  }
  if (offset + n > MAX_FILE_SIZE)
    n = MAX_FILE_SIZE - offset;

  if (n > 0 && inode_write_data(&node, offset, buf, n) < 0) {
    fs_leave();
    return -1;                   /* disk full */
  }

  if (n > 0 && offset + n > node.size)
    node.size = offset + n;
  write_inode(fds[fd].inode, &node);
  fds[fd].offset += n;

  fs_leave();
  return n;
}

int fs_lseek(int fd, int offset)
{
  fs_enter();

  if (!fs_ready || !fd_check(fd) || offset < 0) {
    fs_leave();
    return -1;
  }
  fds[fd].offset = offset;

  fs_leave();
  return 0;
}

int fs_mkdir(char *fileName)
{
  int parent, target, ino, block, i;
  char name[MAX_FILE_NAME], abs[MAX_PATH_NAME];
  inode_t node;

  fs_enter();

  if (!fs_ready || resolve_path(fileName, &parent, &target, name, abs) < 0) {
    fs_leave();
    return -1;
  }
  if (target >= 0 || parent < 0 || !name_ok(name)) {
    fs_leave();
    return -1;                   /* exists already / bad name */
  }

  ino = inode_alloc();
  block = (ino >= 0) ? data_alloc() : -1;
  if (ino < 0 || block < 0) {
    if (ino >= 0)
      inode_free(ino);
    fs_leave();
    return -1;                   /* out of inodes or disk full */
  }

  node.type = DIRECTORY;
  node.links = 1;
  node.size = 0;
  for (i = 0; i < 8; ++i)
    node.dir[i] = 0;
  node.dir[0] = block;
  write_inode(ino, &node);

  if (dir_add_entry(parent, name, ino) < 0) {
    free_blocks(&node);
    inode_free(ino);
    fs_leave();
    return -1;
  }

  fs_leave();
  return 0;
}

int fs_rmdir(char *fileName)
{
  int parent, target, i, n;
  char name[MAX_FILE_NAME], abs[MAX_PATH_NAME];
  inode_t node;

  fs_enter();

  if (!fs_ready || resolve_path(fileName, &parent, &target, name, abs) < 0) {
    fs_leave();
    return -1;
  }
  if (target < 0 || target == ROOT_INODE) {
    fs_leave();
    return -1;
  }
  read_inode(target, &node);
  if (node.type != DIRECTORY) {
    fs_leave();
    return -1;
  }
  /* Never remove the cwd or a directory above it */
  if (path_inside(abs, cwd)) {
    fs_leave();
    return -1;
  }
  n = node.size / (int) sizeof(dirent_t);
  if (n > 0) {
    fs_leave();
    return -1;                   /* not empty */
  }

  /* Drop it from the parent directory and free its resources */
  if (dir_remove_entry(parent, name) < 0) {
    fs_leave();
    return -1;
  }
  for (i = 0; i < 8; ++i)
    if (node.dir[i] != 0)
      data_free(node.dir[i]);
  inode_free(target);

  fs_leave();
  return 0;
}

int fs_cd(char *dirName)
{
  int parent, target;
  char name[MAX_FILE_NAME], abs[MAX_PATH_NAME];
  inode_t node;
  int i;

  fs_enter();

  if (!fs_ready || resolve_path(dirName, &parent, &target, name, abs) < 0) {
    fs_leave();
    return -1;
  }
  if (target < 0) {
    fs_leave();
    return -1;
  }
  read_inode(target, &node);
  if (node.type != DIRECTORY) {
    fs_leave();
    return -1;
  }

  /* Install the new cwd */
  for (i = 0; i < MAX_PATH_NAME; ++i)
    cwd[i] = '\0';
  for (i = 0; abs[i] != '\0' && i < MAX_PATH_NAME - 1; ++i)
    cwd[i] = abs[i];
  cwd[i] = '\0';

  fs_leave();
  return 0;
}

int fs_link(char *old_fileName, char *new_fileName)
{
  int parent, target, newparent, newtarget;
  char name[MAX_FILE_NAME], abs[MAX_PATH_NAME];
  char newname[MAX_FILE_NAME], newabs[MAX_PATH_NAME];
  inode_t node;

  fs_enter();

  if (!fs_ready) {
    fs_leave();
    return -1;
  }
  if (resolve_path(old_fileName, &parent, &target, name, abs) < 0 ||
      target < 0) {
    fs_leave();
    return -1;
  }
  read_inode(target, &node);
  if (node.type == DIRECTORY) {
    fs_leave();
    return -1;                   /* cannot hard-link directories */
  }
  if (resolve_path(new_fileName, &newparent, &newtarget, newname,
          newabs) < 0 ||
      newparent < 0 || newtarget >= 0 || !name_ok(newname)) {
    fs_leave();
    return -1;                   /* destination exists or is invalid */
  }
  if (dir_add_entry(newparent, newname, target) < 0) {
    fs_leave();
    return -1;
  }
  node.links = node.links + 1;
  write_inode(target, &node);

  fs_leave();
  return 0;
}

int fs_unlink(char *fileName)
{
  int parent, target;
  char name[MAX_FILE_NAME], abs[MAX_PATH_NAME];
  inode_t node;

  fs_enter();

  if (!fs_ready || resolve_path(fileName, &parent, &target, name, abs) < 0 ||
      target < 0 || parent < 0) {
    fs_leave();
    return -1;
  }
  read_inode(target, &node);
  if (node.type == DIRECTORY) {
    fs_leave();
    return -1;                   /* use rmdir on directories */
  }
  if (dir_remove_entry(parent, name) < 0) {
    fs_leave();
    return -1;
  }

  node.links = node.links - 1;
  if (node.links <= 0) {
    /* Last link gone: release the file's resources */
    free_blocks(&node);
    node.type = FREE_INODE;
    node.links = 0;
    node.size = 0;
  }
  write_inode(target, &node);

  fs_leave();
  return 0;
}

int fs_stat(char *fileName, fileStat *buf)
{
  int parent, target;
  char name[MAX_FILE_NAME], abs[MAX_PATH_NAME];
  inode_t node;
  fileStat s;

  fs_enter();

  if (!fs_ready || resolve_path(fileName, &parent, &target, name, abs) < 0 ||
      target < 0) {
    fs_leave();
    return -1;
  }
  read_inode(target, &node);

  s.inodeNo = target;
  s.type = (short) node.type;
  s.links = (char) node.links;
  s.size = node.size;
  s.numBlocks = count_blocks(&node);

  /* 'buf' points into the caller's (user) address space */
  bcopy((char *) &s, (char *) buf, sizeof(fileStat));

  fs_leave();
  return 0;
}
