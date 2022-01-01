#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifndef DISK_DRIVER_H
#include "disk_driver.h"
extern struct disk_operations disk_ops;
#endif

#ifndef FFS_SUPER_H
#include "ffs_super.h"
extern struct super_operations super_ops;
#endif

#ifndef FFS_INODE_H
#include "ffs_inode.h"
extern struct inode_operations inode_ops;
#endif

#ifndef FFS_DIR_H
#include "bfs_dir.h"
#endif

static struct dir root, other, *cwd = &root;

/* ---------------- Some helper functions ---------------- */

// Requires target str size > 4
static void name2str(char *str, char *nam)
{
  memset(str, 0, FNAME_LENGTH + 1);
  memcpy(str, nam, 4);
}

static int getfree()
{
  for (int i = 0; i < DENTRIES_PER_BLOCK; i++)
  {
    if (cwd->dirBlk.dir[i].name[0] == '\0')
      return i;
  }

  return -ENOSPC;
}

// requires name NULL terminated
// returns -1 if not found
static int findname(char *name)
{
  char str[FNAME_LENGTH + 1];

  if (!cwd->dbOpen)
    return -ENOTDIR;

  for (int i = 0; i < DENTRIES_PER_BLOCK; i++)
  {
    name2str(str, cwd->dirBlk.dir[i].name);
    if (!(strcmp(str, name)))
      return i;
  }

  return -ENOENT;
}

/* ---------------- End of helper functions ---------------- */

/***
  Open a directory and read-in the directory block
  MUST O / first, and cwd -> root
  then, either C / OR O another, and cwd -> other
  if another is open, MUST C another and then C /
***/
int dir_open(char *name)
{
  int ercode;
  unsigned int blkOffset = ROOT_DIR_OFFSET;

  if (!strlen(name) || (strlen(name) > 4))
    return -EINVAL;

  if (!strcmp(name, "/"))
  { // Root Dir
    ercode = disk_ops.read(blkOffset, root.dirBlk.data);
    if (ercode < 0)
      return ercode;

    cwd = &root;
  }
  else
  {
    ercode = findname(name);
    if(ercode < 0)
      return ercode;

    int inode = cwd->dirBlk.dir[ercode].inode;

    union sml_lrg in;
    ercode = inode_ops.read(inode, &in);

    if(in.smlino.type != 'D')
      return -ENOTDIR;

    blkOffset = super_ops.getStartDtArea() + in.smlino.start;

    ercode = disk_ops.read(blkOffset, other.dirBlk.data);

    cwd = &other;
  }

  cwd->dbOpen = 1;
  cwd->lastEntry = 0;
  cwd->diskBlk = blkOffset;

  return 0;
}

/***
  Close the current directory and write-out the directory block
  It's the user responsability to assert that the name corresponds to
  the currently opened directory
***/
int dir_close(char *name)
{
  int ercode;

  if (!strlen(name) || (strlen(name) > 4))
    return -EINVAL;
  if (!cwd->dbOpen)
    return -ENOTDIR;

  ercode = disk_ops.write(cwd->diskBlk, cwd->dirBlk.data);
  if (ercode < 0)
    return ercode;

  cwd->dbOpen = 0;

  if (strcmp(name, "/")) // if closed directory was diferent from root
    cwd = &root;

  cwd->lastEntry = 0;

  return 0;
}

/***
  Create a new dentry into a free directory slot, if any
    parameters:
     @in: requires name NULL terminated, number of inode to assign
    BUT does not set or check anything
    errors:
     -ENOTDIR directory not open
     -EINVAL name not correctly formatted
     -EEXIST an entry with that name already exists
     -ENOSPC no free slot for new entry					***/

int dir_create(char *name, unsigned int inode)
{
  int ercode;

  if (!cwd->dbOpen)
    return -ENOTDIR;

  if (!strlen(name) || (strlen(name) > 4))
    return -EINVAL;

  ercode = findname(name);
  if (ercode >= 0)
    return -EEXIST;

  ercode = getfree();
  if (ercode < 0)
    return ercode;

  // Copy the data to the correct location (which is ercode)
  memcpy(cwd->dirBlk.dir[ercode].name, name, 4);
  cwd->dirBlk.dir[ercode].inode = inode;

  return 0;
}

/***
  Delete a dentry from the directory
    parameters:
      @in: requires name NULL terminated, number of inode to assign
     BUT does not set or check anything
     @out: returns number of inode to delete
    errors:
     -ENOTDIR directory not open
     -EINVAL name not correctly formatted
     -ENOENT no entry with that name					***/

int dir_delete(char *name)
{
  int ercode, inode2del;

  if (!cwd->dbOpen)
    return -ENOTDIR;

  if (!strlen(name) || (strlen(name) > 4))
    return -EINVAL;

  ercode = findname(name);
  if (ercode < 0)
    return -ENOENT;

  // Retrieve the inode from the dentry we're goind to "erase"
  inode2del = cwd->dirBlk.dir[ercode].inode;
  memset(&cwd->dirBlk.dir[ercode], 0, sizeof(struct dentry));

  return inode2del;
}

/***
  Get the next (valid) file entry
    parameters:
      @in/out:  index for entry access;
    set to 0 on open() or with rewinddir()
      @out: returns pointer to dentry, NULL at last entry
      or error, setting errno
    errors:
     -ENOTDIR directory not open					 ***/

struct dentry *dir_read()
{

  if (!cwd->dbOpen)
  {
    errno = -ENOTDIR;
    return NULL;
  } // just like readdir()

  int i = cwd->lastEntry;
  while (i < DENTRIES_PER_BLOCK)
  {
    if (cwd->dirBlk.dir[i].name[0] != '\0')
    {
      cwd->lastEntry = i;
      return &(cwd->dirBlk.dir[i]);
    }
    i++;
  }

  return NULL;
}

/***
  Reposition the pointer used by dir_read to zero
    parameters:
      @out: returns pointer to the beggining (entry 0)
    errors:
     -ENOTDIR directory not open					 ***/

int dir_rewind()
{

  if (!cwd->dbOpen)
    return -ENOTDIR;
  cwd->lastEntry = 0;

  return 0;
}

// ** MORE Helper functions **

int dir_print_table(int all)
{
  int left = DENTRIES_PER_BLOCK;
  char str[FNAME_LENGTH + 1];

  if (!cwd->dbOpen)
    return -ENOTDIR;

  printf("Printing %s directory entries -----------\n", (all ? "all" : "valid"));

  for (int i = 0; i < left; i++)
  {
    name2str(str, cwd->dirBlk.dir[i].name);
    if (all)
      printf("%02d: %4s %02d\n", i, str, cwd->dirBlk.dir[i].inode);
    else if (strlen(str))
      printf("%02d: %4s %02d\n", i, str, cwd->dirBlk.dir[i].inode);
  }

  return 0;
}

int readdir_print_table()
{
  int ercode;
  struct dentry *dePtr;
  char str[FNAME_LENGTH + 1];

  ercode = dir_rewind();
  if (ercode < 0)
    return -ENOTDIR;

  printf("Printing valid directory entries -----------\n");

  /*** TODO: use readdir to print the entries (same format as above) ***/
  dePtr = dir_read();
  while (dePtr != NULL)
  {
    name2str(str, dePtr->name);
    printf("%02d: %4s %02d\n", cwd->lastEntry, str, dePtr->inode);
    cwd->lastEntry++;
    dePtr = dir_read();
  }

  return errno; // if there was no error, errno == 0
}

struct dir_operations dir_ops = {
    .open = dir_open,
    .close = dir_close,
    .readdir = dir_read,
    .rewinddir = dir_rewind,
    .create = dir_create,
    .deletedir = dir_delete,
    .getfree = getfree,
    .findname = findname};
