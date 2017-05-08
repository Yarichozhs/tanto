#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/statfs.h>
#include <redislib.h>
#include <ytrace.h>

#define TANTO_PATH_MAXLEN (512)
#define TANTO_KEY_MAXLEN  (512)
#define TANTO_NAME_MAX    (256)
#define TANTO_BLOCK_SIZE  (4 * 1024)

#define tanto_block_align(size) \
        ( ((size) + (TANTO_BLOCK_SIZE - 1)) & ~(TANTO_BLOCK_SIZE - 1))

/* File object in backend */
struct tanto_fobj_t
{
  int32_t   seqno;
  mode_t    mode;
  uid_t     uid;
  gid_t     gid;
  int32_t   nblocks;
  int32_t   bsize;
};
typedef struct tanto_fobj_t tanto_fobj_t;

/* Directory object in directory file data */
struct tanto_dobj_t
{
  uint32_t  seqno;
  uint32_t  flags;
  char      name[TANTO_NAME_MAX];
};
typedef struct tanto_dobj_t tanto_dobj_t;

/* Runtime file handle */
struct tanto_file_t
{
  char          path[TANTO_PATH_MAXLEN];
  char          key[TANTO_KEY_MAXLEN];
  int           keyl;
  tanto_fobj_t  fobj;
};
typedef struct tanto_file_t tanto_file_t;

#define TANTO_BLOCK_DIR_MAX (TANTO_BLOCK_SIZE / sizeof(tanto_dobj_t))

#define tanto_stat_key(key, path) \
        sprintf(key, "%s@fobj", path)

#define tanto_data_key(key, path, ind) \
        sprintf(key, "%s@data::%lld", path, (signed long long)ind)

struct tanto_ctx_t
{
  redis_ctx_t redis_ctx;
};
typedef struct tanto_ctx_t tanto_ctx_t;

__thread tanto_ctx_t tanto_ctx;

#define tanto_redis_ctx() (&tanto_ctx.redis_ctx)

static int tanto_add_obj(const char *path, mode_t mode, uid_t uid, gid_t gid)
{
  tanto_fobj_t fobj;
  char         key[TANTO_KEY_MAXLEN];
  int          keyl;
  
  memset(&fobj, 0, sizeof(&fobj));

  keyl = tanto_stat_key(key, path);

  fobj.mode  = mode;
  fobj.uid   = uid;
  fobj.gid   = gid;
  fobj.seqno = ytime_get()/1000/1000;                  /* convert to seconds */

  if (redis_set(tanto_redis_ctx(), key, keyl, 
                (void *)&fobj, sizeof(tanto_fobj_t)) < 0)
  {
    ytrace_msg(YTRACE_ERROR, "set %s failed\n", key);
    return -ENOMEM;
  }

  return 0;
}

static int tanto_get_obj(tanto_fobj_t *fobj, const char *path)
{
  char key[TANTO_KEY_MAXLEN];
  int  keyl;

  ytrace_msg(YTRACE_LEVEL1, "path = %s\n", path);

  keyl = tanto_stat_key(key, path);

  if (redis_get(tanto_redis_ctx(), key, keyl, 
                (void *)fobj, sizeof(tanto_fobj_t)) < 0) 
  {
    ytrace_msg(YTRACE_LEVEL1, "redis key get [%s][%d] failed\n",
               key, keyl);
    return -ENOENT;
  }
  
  return 0;
}

static int tanto_file_get(tanto_file_t *file, const char *path)
{
  ytrace_msg(YTRACE_LEVEL1, "path = %s\n", path);

  strcpy(file->path, path);

  file->keyl = tanto_stat_key(file->key, path);

  if (redis_get(tanto_redis_ctx(), file->key, file->keyl, 
                (void *)&file->fobj, sizeof(tanto_fobj_t)) < 0) 
  {
    ytrace_msg(YTRACE_LEVEL1, "redis key get [%s][%d] failed\n",
               file->key, file->keyl);
    return -ENOENT;
  }
  
  return 0;
}

static int tanto_file_sync(tanto_file_t *file)
{
  ytrace_msg(YTRACE_LEVEL1, "path = %s\n", file->path);

  if (redis_set(tanto_redis_ctx(), file->key, file->keyl, 
                (void *)&file->fobj, sizeof(tanto_fobj_t)) < 0) 
  {
    ytrace_msg(YTRACE_LEVEL1, "redis key get [%s][%d] failed\n",
               file->key, file->keyl);
    return -ENOENT;
  }
  
  return 0;
}

static int tanto_file_read(tanto_file_t *file,
                           size_t blk_ind, void *data, size_t datal)
{
  char key[TANTO_KEY_MAXLEN];
  int  keyl;

  ytrace_msg(YTRACE_LEVEL1, "block_ind = %lu\n", (unsigned long)blk_ind);

  keyl = tanto_data_key(key, file->path, blk_ind);

  if (redis_get(tanto_redis_ctx(), key, keyl, data, datal) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "redis key get [%s][%d] failed\n", key, keyl);
    return -ENOENT;
  }
  
  return 0;
}

static int tanto_file_write(tanto_file_t *file, 
                            void *data, size_t size, size_t offset)
{
  int     keyl;
  void   *bp;
  size_t  blk_ind;
  size_t  noffset;
  size_t  ioffset;
  size_t  tsize;
  size_t  rem = size;
  char    key[TANTO_KEY_MAXLEN];
  char    ldata[TANTO_BLOCK_SIZE];
  size_t  nblocks = 0;

  ytrace_msg(YTRACE_LEVEL1, "block_ind = %lu\n", (unsigned long)blk_ind);

  while (rem)
  {
    noffset = (offset + TANTO_BLOCK_SIZE) & ~(TANTO_BLOCK_SIZE - 1);
    
    tsize   = noffset - offset;
    blk_ind = offset / TANTO_BLOCK_SIZE;

    keyl    = tanto_data_key(key, file->path, blk_ind);
    bp      = data;

    if (rem < tsize)
      tsize = rem;

    if (tsize != TANTO_BLOCK_SIZE)
    {
      ioffset = offset - blk_ind * TANTO_BLOCK_SIZE;

      if (redis_get(tanto_redis_ctx(), key, keyl, ldata, TANTO_BLOCK_SIZE) < 0)
        memset(ldata, 0, TANTO_BLOCK_SIZE);

      memcpy(&ldata[ioffset], (char *)bp, tsize);

      bp = ldata;
    }

    if (redis_set(tanto_redis_ctx(), key, keyl, bp, TANTO_BLOCK_SIZE) < 0) 
    {
      ytrace_msg(YTRACE_LEVEL1, "redis key get [%s][%d] failed\n", key, keyl);
      return -ENOENT;
    }

    nblocks++;

    rem    -= tsize;
    offset += tsize;
    data    = (void *)((char *)data + tsize);
  }

  if (file->fobj.nblocks < nblocks)                 /* update on size change */
  {
    file->fobj.nblocks = nblocks;
    tanto_file_sync(file);
  }

  return 0;
}

static int tanto_file_del(tanto_file_t *file)
{
  size_t  ind;
  char    key[TANTO_KEY_MAXLEN];
  int     keyl;

  ytrace_msg(YTRACE_LEVEL1, "delete file = %s\n", file->path);

  /* First remove the entry */
  redis_del(tanto_redis_ctx(), file->key, file->keyl);

  for (ind = 0; ind < file->fobj.nblocks; ind++)
  {
    keyl = tanto_data_key(key, file->path, ind);

    redis_del(tanto_redis_ctx(), key, keyl);
  }

  return 0;
}

static int tanto_dir_add_file(tanto_file_t *dfile, const char *path)
{
  char key[TANTO_KEY_MAXLEN];
  int  keyl;
  int  ind;
  int  ind2;
  char  data[TANTO_BLOCK_SIZE];
  tanto_dobj_t *dobj;
  tanto_fobj_t *dir = &dfile->fobj;

  ytrace_msg(YTRACE_LEVEL1, "%s: path = %s\n", __func__, path);

  for (ind = 0; ind < dir->nblocks; ind++)
  {
    keyl = tanto_data_key(key, dfile->path, ind);

    if (redis_get(tanto_redis_ctx(), key, keyl, 
                  (void *)data, sizeof(data)) < 0)
    {
      ytrace_msg(YTRACE_LEVEL1, "tanto_dir_add_file : block read failed\n");
      return -ENOENT;
    }

    dobj = (tanto_dobj_t *)data;
    
    for (ind2 = 0; ind2 < TANTO_BLOCK_DIR_MAX; ind2++)
    {
      if (dobj[ind2].name[0] == '\0')                          /* free entry */
      {
        strcpy(dobj[ind2].name, path);

        if (redis_set(tanto_redis_ctx(), key, keyl, 
	              (void *)data, sizeof(data)) < 0)
        {
          ytrace_msg(YTRACE_LEVEL1, "block read failed\n");
          return -ENOENT;
        }

	return 0;                                   /* reused an entry, done */
      }
    }
  }

  /* No free entry found. Add another block */
  keyl = tanto_data_key(key, dfile->path, ind);

  ytrace_msg(YTRACE_LEVEL1, "Adding new entry to dir [%s] : [%s] \n",
             dfile->path, path);

  memset(data, 0, sizeof(data));

  dobj = (tanto_dobj_t *)data;
  strcpy(dobj[0].name, path);

  if (redis_set(tanto_redis_ctx(), key, keyl, (void *)data, sizeof(data)) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "tanto_dir_add_file : new block add failed\n");
    return -ENOMEM;
  }

  dir->nblocks++;                                       /* add another block */

  ytrace_msg(YTRACE_LEVEL1, "new block count [%s] : [%d] \n",
             dfile->path, dir->nblocks);

  if (redis_set(tanto_redis_ctx(), dfile->key, dfile->keyl, 
                (void *)dir, sizeof(tanto_fobj_t)) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "tanto_dir_add_file : meta update failed\n");
    return -ENOMEM;
  }

  return 0;
}

static int tanto_dir_del_file(tanto_file_t *dfile, const char *path)
{
  char key[TANTO_KEY_MAXLEN];
  int  keyl;
  int  ind;
  int  ind2;
  char  data[TANTO_BLOCK_SIZE];
  tanto_dobj_t *dobj;
  tanto_fobj_t *dir = &dfile->fobj;

  ytrace_msg(YTRACE_LEVEL1, "%s: path = %s\n", __func__, path);

  for (ind = 0; ind < dir->nblocks; ind++)
  {
    keyl = tanto_data_key(key, dfile->path, ind);

    if (redis_get(tanto_redis_ctx(), key, keyl, 
                  (void *)data, sizeof(data)) < 0)
    {
      ytrace_msg(YTRACE_LEVEL1, "block read failed\n");
      return -ENOENT;
    }

    dobj = (tanto_dobj_t *)data;
    
    for (ind2 = 0; ind2 < TANTO_BLOCK_DIR_MAX; ind2++)
    {
      if (dobj[ind2].name[0] == '\0')                          /* free entry */
        continue;

      if (strcmp(dobj[ind2].name, path) == 0)
      {
        ytrace_msg(YTRACE_LEVEL1, "removing file %s in dir %s\n",
	           path, dfile->path);

	dobj[ind2].name[0] = 0;
	dobj[ind2].flags = 0;
	dobj[ind2].seqno = 0;

        /* Sync back to backend */
        redis_set(tanto_redis_ctx(), key, keyl, (void *)data, sizeof(data));

	return 0;             
      }
    }
  }

  ytrace_msg(YTRACE_LEVEL1, "file %s not found in dir %s\n", path, dfile->path);

  return -ENOENT;
}

int tanto_split_name(const char *path, char *dir, int dirl, 
                     char *base, int basel)
{
  const char *bp;
  const char *lp;

  bp = lp = path;

  while (*bp)
  {
    if (*bp == '/')
    {
      if (bp[1])                              /* assume normalized file name */
        lp = bp + 1;
    }

    bp++;
  }

  dirl = lp - path;

  strncpy(dir, path, dirl);
  dir[dirl] = 0;

  if (dirl > 1 && dir[dirl - 1] == '/')
    dir[dirl - 1] = 0;

  strncpy(base, &path[dirl], basel);

  ytrace_msg(YTRACE_LEVEL1, "tanto_split_name : [%s] [%s] [%s]\n", 
             path, dir, base);
}

static void tanto_init()
{
  tanto_file_t file;

  if (redis_connect(tanto_redis_ctx(), NULL, 0) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "thread [%ld] : redis connect failed\n",
               (long int)pthread_self());
    exit(0);
  }

  if (tanto_file_get(&file, "/") < 0)
    tanto_add_obj("/", S_IFDIR|0755, 0, 0);
}


/*---------------------------------------------------------------------------*
 *                            FUSE CALLBACKS                                 *
 *---------------------------------------------------------------------------*/

static int tanto_getattr(const char *path, struct stat *stbuf)
{
  tanto_fobj_t *fobj;
  tanto_file_t  file;

  ytrace_msg(YTRACE_LEVEL1, "path = %s\n", path);

  if (tanto_file_get(&file, path) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "file get [%s] failed\n", path);
    return -ENOENT;
  }

  memset(stbuf, 0, sizeof(*stbuf));

  fobj = &file.fobj;

  stbuf->st_dev   = 0x12345678;
  stbuf->st_ino   = fobj->seqno;
  stbuf->st_nlink = 1;
  stbuf->st_mode  = fobj->mode;
  stbuf->st_uid   = fobj->uid;
  stbuf->st_gid   = fobj->gid;
  stbuf->st_size  = fobj->nblocks * TANTO_BLOCK_SIZE;
  stbuf->st_blksize = TANTO_BLOCK_SIZE;
  stbuf->st_blocks  = fobj->nblocks ;

  ytrace_msg(YTRACE_LEVEL1, "seq = %d : mode = %o : size = %lu\n",
             fobj->seqno, fobj->mode, fobj->nblocks * TANTO_BLOCK_SIZE);

  return 0;
}

static int tanto_readlink(const char *path, char *buf, size_t size)
{
  /* TANTO TODO :  */
  return -ENOENT;
}

static int tanto_getdir(const char *path, fuse_dirh_t h, fuse_dirfil_t filler)
{
  int           ind;
  int           ind2;
  char          data[TANTO_BLOCK_SIZE];
  tanto_file_t  file;
  tanto_fobj_t *fobj;
  tanto_dobj_t *dobj;

  ytrace_msg(YTRACE_LEVEL1, "path = %s\n", path);

  if (tanto_file_get(&file, path) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "dir obj get %s failed\n", path);
    return -ENOENT;
  }

  fobj = &file.fobj;

  ytrace_msg(YTRACE_LEVEL1, "Number of blocks = %d\n", fobj->nblocks);

  for (ind = 0; ind < fobj->nblocks; ind++)
  {
    if (tanto_file_read(&file, ind, (void *)data, sizeof(data)) < 0)
    {
      ytrace_msg(YTRACE_LEVEL1, "dir block read failed\n");
      return -ENOENT;
    }

    dobj = (tanto_dobj_t *)data;
    
    for (ind2 = 0; ind2 < TANTO_BLOCK_DIR_MAX; ind2++)
    {
      if (dobj[ind2].name[0] == '\0')                     /* skip free entry */
        continue;

      ytrace_msg(YTRACE_LEVEL1, "returning file (%d %d) : %s\n",
                 ind, ind2, dobj[ind2].name);

      filler(h, dobj[ind2].name, S_IFREG, dobj[ind2].seqno);
    }
  }

  return 0;
}

static int tanto_mknod(const char *path, mode_t mode, dev_t rdev)
{
  char         base[TANTO_PATH_MAXLEN];
  char         dir[TANTO_PATH_MAXLEN];
  tanto_file_t file;
  struct fuse_context *fctx = fuse_get_context();

  tanto_split_name(path, dir, sizeof(dir), base, sizeof(base));

  ytrace_msg(YTRACE_LEVEL1, "path = %s %o %d [base = %s]\n", 
             path, mode, (int)rdev, base);

  /* Check if directory exists */
  if (tanto_file_get(&file, dir) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "tanto_mknod : dir obj get failed\n");
    return -ENOENT;
  }

  /* Add a file entry first into directory */
  if (tanto_dir_add_file(&file, base) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "tanto_mknod : add new path to dir failed \n");
    return -ENOMEM;
  }

  /* Add the file object for new element */
  if (tanto_add_obj(path, mode, fctx->uid, fctx->gid) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "tanto_mknod : add new path failed \n");
    return -ENOMEM;
  }

  ytrace_msg(YTRACE_LEVEL1, "path = %s done \n", path);
  return 0;
}

static int tanto_mkdir(const char *path, mode_t mode)
{
  int ret;

  ytrace_msg(YTRACE_LEVEL1, "path = %s %o\n", path, mode);

  ret = tanto_mknod(path, S_IFDIR|mode, 0);

  return ret;
}

static int tanto_unlink(const char *path)
{
  tanto_file_t file;
  tanto_file_t dfile;
  char         base[TANTO_PATH_MAXLEN];
  char         dir[TANTO_PATH_MAXLEN];

  tanto_split_name(path, dir, sizeof(dir), base, sizeof(base));

  if (tanto_file_get(&file, path) < 0)
    return -ENOENT;

  if (tanto_file_get(&dfile, dir) < 0)
    return -ENOENT;
  
  if (tanto_dir_del_file(&dfile, base) < 0)
    return -EINVAL;

  if (tanto_file_del(&file) < 0)
    return -ENOENT;

  ytrace_msg(YTRACE_LEVEL1, "unlinked = %s\n", path);

  return 0;
}

static int tanto_rmdir(const char *path)
{
  tanto_file_t file;
  tanto_file_t dfile;
  char         base[TANTO_PATH_MAXLEN];
  char         dir[TANTO_PATH_MAXLEN];

  ytrace_msg(YTRACE_LEVEL1, "rmdir = %s\n", path);

  tanto_split_name(path, dir, sizeof(dir), base, sizeof(base));

  if (tanto_file_get(&file, path) < 0)
    return -ENOENT;

  if (tanto_file_get(&dfile, dir) < 0)
    return -ENOENT;
  
  if (tanto_dir_del_file(&dfile, base) < 0)
    return -EINVAL;

  if (tanto_file_del(&file) < 0)
    return -ENOENT;

  ytrace_msg(YTRACE_LEVEL1, "rmdir done = %s\n", path);

  return 0;
}

static int tanto_symlink(const char *from, const char *to)
{
  /* TANTO : TODO */
  ytrace_msg(YTRACE_LEVEL1, "path = %s\n", from);
  return -ENOENT;
}

static int tanto_rename(const char *from, const char *to)
{
  /* TANTO : TODO */
  ytrace_msg(YTRACE_LEVEL1, "path = %s\n", from);
  return -ENOENT;
}

static int tanto_link(const char *from, const char *to)
{
  /* TANTO : TODO */
  return -ENOENT;
}

static int tanto_chmod(const char *path, mode_t mode)
{
  /* TANTO : TODO */
  ytrace_msg(YTRACE_LEVEL1, "path = %s %d\n", path, mode);
  return -ENOENT;
}

static int tanto_chown(const char *path, uid_t uid, gid_t gid)
{
  /* TANTO : TODO */
  ytrace_msg(YTRACE_LEVEL1, "path = %s : %d %d \n", path, uid, gid);
  return -ENOENT;
}

static int tanto_truncate(const char *path, off_t size)
{
  int           ret;
  size_t        nblocks;
  tanto_file_t  file;
  tanto_fobj_t *fobj;
  
  ytrace_msg(YTRACE_LEVEL1, "path = %s : size = %ld\n", path, (long)size);

  size    = tanto_block_align(size);
  nblocks = size / TANTO_BLOCK_SIZE;

  if (tanto_file_get(&file, path) < 0)
    return -ENOENT;

  fobj = &file.fobj;

  if (fobj->nblocks > nblocks)
  {
    /* TODO : release blocks */
  }

  fobj->nblocks = nblocks;

  if (tanto_file_sync(&file) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "%s: file sync failed\n", __func__);
    return -ENOMEM;
  }

  return 0;
}

static int tanto_utime(const char *path, struct utimbuf *buf)
{
  int          ret;
  tanto_fobj_t fobj;

  ytrace_msg(YTRACE_LEVEL1, "%s: path = %s\n", __func__, path);

  ret = tanto_get_obj(&fobj, path);

  if (ret != 0)
    return ret;

  buf->actime  = 0;
  buf->modtime = 0;

  /* TANTO : TODO */

  return ret;
}

static int tanto_open(const char *path, struct fuse_file_info *finfo)
{
  int          ret;
  tanto_file_t file;

  ret = tanto_file_get(&file, path);

  ytrace_msg(YTRACE_LEVEL1, "path = %s : ret = %d\n", path, ret);

  return ret;
}

static int tanto_read(const char *path, char *buf, size_t size, off_t offset, 
                      struct fuse_file_info *finfo)
{
  int          ret;
  int          ind;
  size_t       blk_ind;
  size_t       blk_cnt;
  size_t       blk_off;
  char        *blk_bp;
  tanto_file_t file;

  if (tanto_file_get(&file, path) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "%s: file get failed\n", __func__);
    return -ENOENT;
  }

  if ((size != tanto_block_align(size)) ||
      (offset != tanto_block_align(offset)))
  {
    ytrace_msg(YTRACE_LEVEL1, "alignment issue %ld : %ld : %d\n", 
               (long)size, (long)offset, TANTO_BLOCK_SIZE);
    return -EINVAL;
  }

  blk_cnt = size   / TANTO_BLOCK_SIZE;
  blk_off = offset / TANTO_BLOCK_SIZE;
  
  ytrace_msg(YTRACE_LEVEL1, "path = %s : size =%ld : offset = %ld\n", 
             path, (long)size, (long)offset);

  for (ind = 0; ind < blk_cnt; ind++)
  {
    blk_bp = &buf[ind * TANTO_BLOCK_SIZE];

    if (tanto_file_read(&file, blk_off + ind, 
                        (void *)blk_bp, TANTO_BLOCK_SIZE) < 0)
    {
      memset(blk_bp, 0, TANTO_BLOCK_SIZE);
    }
  }

  ytrace_msg(YTRACE_LEVEL1, "%s: read completed successfully\n", __func__);

  return size;
}

static int tanto_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *finfo)
{
  tanto_file_t file;

  ytrace_msg(YTRACE_LEVEL1, "path = %s : size =%ld : offset = %ld\n", 
             path, (long)size, (long)offset);

  if (tanto_file_get(&file, path) < 0)
  {
    ytrace_msg(YTRACE_LEVEL1, "%s: file get failed\n", __func__);
    return -ENOENT;
  }

  if (tanto_file_write(&file, (void *)buf, size, offset) < 0)
      return -EINVAL;

  ytrace_msg(YTRACE_LEVEL1, "write completed successfully\n");

  return size;
}

static int tanto_statfs(const char *path, struct statvfs *fst)
{
  /* TODO */
  return -1;
}

static int tanto_release(const char *path, struct fuse_file_info *finfo)
{
    /* Just a stub.  This method is optional and can safely be left
       unimplemented */

    (void) path;
    (void) finfo;
    return 0;
}

static int tanto_fsync(const char *path, int isdatasync,
                       struct fuse_file_info *finfo)
{
    /* Just a stub.  This method is optional and can safely be left
       unimplemented */

    ytrace_msg(YTRACE_LEVEL1, "%s: path = %s\n", __func__, path);
    (void) path;
    (void) isdatasync;
    return 0;
}

static struct fuse_operations tanto_oper = {
    .getattr	= tanto_getattr,
    .readlink	= tanto_readlink,
    .getdir	= tanto_getdir,
    .mknod	= tanto_mknod,
    .mkdir	= tanto_mkdir,
    .symlink	= tanto_symlink,
    .unlink	= tanto_unlink,
    .rmdir	= tanto_rmdir,
    .rename	= tanto_rename,
    .link	= tanto_link,
    .chmod	= tanto_chmod,
    .chown	= tanto_chown,
    .truncate	= tanto_truncate,
    .utime	= tanto_utime,
    .open	= tanto_open,
    .read	= tanto_read,
    .write	= tanto_write,
    .statfs	= tanto_statfs,
    .release	= tanto_release,
    .fsync	= tanto_fsync
    
};

int main(int argc, char *argv[])
{
  tanto_init();

  fuse_main(argc, argv, &tanto_oper, NULL);

  return 0;
}
