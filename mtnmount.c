#define FUSE_USE_VERSION 28
#include "mtnfs.h"
#include "common.h"
#include <fuse.h>

pthread_mutex_t *mtn_rmutex;
pthread_mutex_t *mtn_wmutex;

static int mtnmount_getattr(const char *path, struct stat *stbuf)
{
  kstat *krt = NULL;
  kstat *kst = NULL;
  struct timeval tv;
  char  d[PATH_MAX];
  char  f[PATH_MAX];
  dirbase(path,d,f);
  lprintf(0,"[debug] %s: CALL dir=%s file=%s\n", __func__, d, f);
  memset(stbuf, 0, sizeof(struct stat));
  if(strcmp(path, "/") == 0) {
    stbuf->st_mode  = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    return(0);
  }
  if(strcmp(path, kopt.mtnstatus_path) == 0) {
    stbuf->st_mode  = S_IFDIR | 0555;
    stbuf->st_nlink = 2;
    return(0);
  }
  if(strcmp(d, kopt.mtnstatus_path) == 0){
    gettimeofday(&tv, NULL);
    stbuf->st_mode  = S_IFREG | 0444;
    stbuf->st_atime = tv.tv_sec;
    stbuf->st_mtime = tv.tv_sec;
    stbuf->st_ctime = tv.tv_sec;
    if(strcmp(f, "members") == 0) {
      stbuf->st_size = mtnstatus_members();
      return(0);
    }
    if(strcmp(f, "debuginfo") == 0) {
      stbuf->st_size = mtnstatus_debuginfo();
      return(0);
    }
    return(-ENOENT);
  }

  krt = get_dircache(d, 0);
  for(kst=krt;kst;kst=kst->next){
    if(strcmp(kst->name, f) == 0){
      memcpy(stbuf, &(kst->stat), sizeof(struct stat));
      break;
    }
  }
  if(!kst){
    if(kst = mtn_stat(path)){
      addstat_dircache(d, kst);
      memcpy(stbuf, &(kst->stat), sizeof(struct stat));
    }
  }
  delstats(krt);
  lprintf(0,"[debug] %s: EXIT\n", __func__);
  return(kst ? 0 : -ENOENT);
}

static int mtnmount_fgetattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
  ktask kt;
  struct timeval tv;
  char  d[PATH_MAX];
  char  f[PATH_MAX];
  dirbase(path,d,f);
  lprintf(0, "[debug] %s: path=%s fh=%d\n", __func__, path, fi->fh);
  if(strcmp(path, "/") == 0) {
    st->st_mode  = S_IFDIR | 0755;
    st->st_nlink = 2;
    return(0);
  }
  if(strcmp(path, kopt.mtnstatus_path) == 0){
    st->st_mode  = S_IFDIR | 0555;
    st->st_nlink = 2;
    return(0);
  }
  if(strcmp(d, kopt.mtnstatus_path) == 0){
    if(fi->fh == 0){
      return(-EBADF);
    }
    gettimeofday(&tv, NULL);
    st->st_mode  = S_IFREG | 0444;
    st->st_size  = strlen((const char *)(fi->fh));
    st->st_atime = tv.tv_sec;
    st->st_mtime = tv.tv_sec;
    st->st_ctime = tv.tv_sec;
    if(strcmp(f, "members") == 0) {
      return(0);
    }
    if(strcmp(f, "debuginfo") == 0) {
      return(0);
    }
    return(-EBADF);
  }
  if(fi->fh == 0){
    lprintf(0, "[error] %s: 1\n", __func__);
    return(-EBADF);
  }
  memset(&kt, 0, sizeof(kt));
  strcpy(kt.path, path);
  kt.type = MTNCMD_GETATTR;
  kt.con  = fi->fh;
  mtn_set_string(path, &(kt.send));
  if(mtn_callcmd(&kt) == -1){
    lprintf(0, "[debug] %s: 2\n", __func__);
    return(-errno);
  }
  if(mtn_get_stat(st, &(kt.recv)) == -1){
    lprintf(0, "[error] %s: 3\n", __func__);
    return(-EACCES);
  }
  lprintf(0, "[debug] %s: 4\n", __func__);
  return(0);
}

static int mtnmount_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
  kstat *krt = NULL;
  kstat *kst = NULL;
  lprintf(0, "[debug] %s: CALL path=%s\n", __func__, path);
  filler(buf, ".",  NULL, 0);
  filler(buf, "..", NULL, 0);
  if(strcmp("/", path) == 0){
    filler(buf, kopt.mtnstatus_name, NULL, 0);
  }
  if(strcmp(path, kopt.mtnstatus_path) == 0){
    filler(buf, "members",   NULL, 0);
    filler(buf, "debuginfo", NULL, 0);
  }else{
    lprintf(0, "[debug] %s: EXEC\n", __func__);
    krt = get_dircache(path,1);
    if(krt == NULL){
      krt = mtn_list(path);
      setstat_dircache(path, krt);
    }
    lprintf(0, "[debug] %s: EXEC\n", __func__);
    for(kst=krt;kst;kst=kst->next){
      lprintf(0, "[debug] %s: name=%s\n", __func__, kst->name);
      filler(buf, kst->name, NULL, 0);
    }
    lprintf(0, "[debug] %s: EXEC\n", __func__);
    delstats(krt);
  }
  lprintf(0, "[debug] %s: EXIT\n", __func__);
  return(0);
}

static int mtnmount_mkdir(const char *path, mode_t mode)
{
  int r;
  lprintf(0,"[debug] %s: CALL path=%s\n", __func__, path);
  r = mtn_mkdir(path);
  lprintf(0,"[debug] %s: EXIT r=%d\n", __func__, r);
  return(r);
}

static int mtnmount_unlink(const char *path)
{
  int r;
  char  d[PATH_MAX];
  char  f[PATH_MAX];
  dirbase(path,d,f);
  lprintf(0, "[debug] %s: CALL path=%s\n", __func__, path);
  r = mtn_rm(path);
  setstat_dircache(d, NULL);
  lprintf(0, "[debug] %s: EXIT r=%d\n", __func__, r);
  return(r);
}

static int mtnmount_rmdir(const char *path)
{
  int  r;
  char d[PATH_MAX];
  char f[PATH_MAX];
  dirbase(path,d,f);
  lprintf(0, "[debug] %s: CALL path=%s\n", __func__, path);
  r = mtn_rm(path);
  setstat_dircache(d, NULL);
  lprintf(0, "[debug] %s: EXIT r=%d\n", __func__, r);
  return(r);
}

static int mtnmount_rename(const char *old_path, const char *new_path)
{
  int  r;
  char d0[PATH_MAX];
  char d1[PATH_MAX];
  dirbase(old_path, d0, NULL);
  dirbase(new_path, d1, NULL);
  lprintf(0, "[debug] %s: CALL old_path=%s new_path=%s\n", __func__, old_path, new_path);
  r = mtn_rename(old_path, new_path);
  setstat_dircache(d0, NULL);
  setstat_dircache(d1, NULL);
  lprintf(0, "[debug] %s: EXIT old_path=%s new_path=%s\n", __func__, old_path, new_path);
  return(r);
}

static int mtnmount_readlink(const char *path, char *buff, size_t size)
{
  lprintf(0, "[debug] %s: path=%s\n", __func__, path);
  return 0;
}

static int mtnmount_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
  int  r = 0;
  char d[PATH_MAX];
  char f[PATH_MAX];
  dirbase(path,d,f);
  lprintf(0, "[debug] %s: CALL path=%s fh=%llu mode=%o\n", __func__, path, fi->fh, mode);
  r = mtn_open(path, fi->flags, mode);
  if(r == -1){
    return(-errno);
  }
  fi->fh = (uint64_t)r;
  lprintf(0, "[debug] %s: EXIT   path=%s fh=%llu\n", __func__, path, fi->fh);
  return(0); 
}

static int mtnmount_open(const char *path, struct fuse_file_info *fi)
{
  int   r = 0;
  char  d[PATH_MAX];
  char  f[PATH_MAX];
  dirbase(path,d,f);
  lprintf(0, "[debug] %s: CALL path=%s d=%s f=%s fh=%llu\n", __func__, path, d, f, fi->fh);
  if(strcmp(kopt.mtnstatus_path, d) == 0){
    if(strcmp("members", f) == 0){
      fi->fh = (uint64_t)get_mtnstatus_members();
    }else if(strcmp("debuginfo", f) == 0){
      fi->fh = (uint64_t)get_mtnstatus_debuginfo();
    }else{
      r = -ENOENT;
    }
  }else{
    r = mtn_open(path, fi->flags, 0);
    fi->fh = (uint64_t)r;
    r = (r == -1) ? -errno : 0;
  }
  lprintf(0, "[debug] %s: EXIT path=%s fh=%llu r=%d\n", __func__, path, fi->fh, r);
  return(r); 
}

static int mtnmount_truncate(const char *path, off_t offset)
{
  ktask kt;
  int   r;
  char  d[PATH_MAX];
  char  f[PATH_MAX];
  dirbase(path,d,f);
  lprintf(0, "%s: path=%s\n", __func__, path);
  if(strcmp(kopt.mtnstatus_path, d) == 0){
    if(strcmp("members", f) == 0){
      r = -EACCES;
    }else if(strcmp("debuginfo", f) == 0){
      r = -EACCES;
    }else{
      r = -ENOENT;
    }
  }else{
    memset(&kt, 0, sizeof(kt));
    strcpy(kt.path, path);
    kt.type   = MTNCMD_TRUNCATE;
    kt.create = 1;
    mtn_set_string(path, &(kt.send));
    mtn_set_int(&offset, &(kt.send), sizeof(offset));
    r = (mtn_callcmd(&kt) == -1) ? -errno : 0;
  }
  return(r);
}

static int mtnmount_release(const char *path, struct fuse_file_info *fi)
{
  int   r = 0;
  char  d[PATH_MAX];
  char  f[PATH_MAX];
  dirbase(path,d,f);
  lprintf(0, "[debug] %s: CALL path=%s fh=%llu\n", __func__, path, fi->fh);
  if(strcmp(kopt.mtnstatus_path, d) == 0){
    if(strcmp("members", f) == 0){
      free((void *)(fi->fh));
      fi->fh = 0;
    }else if(strcmp("debuginfo", f) == 0){
      free((void *)(fi->fh));
      fi->fh = 0;
    }else{
      r = -EBADF;
    }
  }else{
    if(fi->fh){
      if(mtn_close(fi->fh) != 0){
        r = -errno;
      }
      fi->fh = 0;
    }
  }
  lprintf(0, "[debug] %s: EXIT path=%s fh=%llu\n", __func__, path, fi->fh);
  return(r);
}

static int mtnmount_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
  int r;
  int l;
  lprintf(0, "[debug] %s: CALL path=%s fh=%llu size=%u offset=%llu\n", __func__, path, fi->fh, size, offset);
  if(memcmp("/.mtnstatus/", path, 12) == 0){
    l = strlen((void *)(fi->fh));
    if(offset > l){
      l = 0;
    }else{
      l -= offset;
    }
    r = (size > l) ? l : size;
    if(r){
      memcpy(buf, (char *)(fi->fh) + offset, r);
    }
  }else{
    pthread_mutex_lock(&(mtn_rmutex[fi->fh]));
    r = mtn_read((int)(fi->fh), buf, size, offset);
    pthread_mutex_unlock(&(mtn_rmutex[fi->fh]));
  }
  lprintf(0, "[debug] %s: EXIT r=%d\n", __func__, r);
  return r;
}

static int mtnmount_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
  int r;
  if(strcmp("/.mtnstatus/members", path) == 0){
    return(-EACCES);
  }
  if(strcmp("/.mtnstatus/debuginfo", path) == 0){
    return(-EACCES);
  }
  pthread_mutex_lock(&(mtn_wmutex[fi->fh]));
  r = mtn_write((int)(fi->fh), buf, size, offset); 
  pthread_mutex_unlock(&(mtn_wmutex[fi->fh]));
  return r;
}

static int mtnmount_chmod(const char *path, mode_t mode)
{
  ktask kt;
  lprintf(0, "[debug] %s: CALL\n", __func__);
  lprintf(0, "%s: path=%s\n", __func__, path);
  memset(&kt, 0, sizeof(kt));
  strcpy(kt.path, path);
  mtn_set_string(path, &(kt.send));
  mtn_set_int32(&mode, &(kt.send));
  if(mtn_callcmd(&kt) == -1){
    return(-errno);
  }
  lprintf(0, "[debug] %s: EXIT\n", __func__);
  return(0);
}

static int mtnmount_chown(const char *path, uid_t uid, gid_t gid)
{
  ktask kt;
  uint32_t val;
  lprintf(0, "[debug] %s: CALL\n", __func__);
  lprintf(0, "%s: path=%s\n", __func__, path);
  memset(&kt, 0, sizeof(kt));
  strcpy(kt.path, path);
  mtn_set_string(path, &(kt.send));
  val = (uint32_t)uid;
  mtn_set_int32(&val, &(kt.send));
  val = (uint32_t)gid;
  mtn_set_int32(&val, &(kt.send));
  kt.type = MTNCMD_CHOWN;
  if(mtn_callcmd(&kt) == -1){
    return(-errno);
  }
  lprintf(0, "[debug] %s: EXIT\n", __func__);
  return(0);
}

static int mtnmount_statfs(const char *path, struct statvfs *sv)
{
  kmember *m;
  kmember *km = mtn_info();
  lprintf(0, "[debug] %s: CALL path=%s\n", __func__, path);
  statvfs("/", sv);
  sv->f_blocks = 0;
  sv->f_bfree  = 0;
  sv->f_bavail = 0;
  for(m=km;m;m=m->next){
    sv->f_blocks += (m->dsize * m->fsize) / sv->f_frsize;
    sv->f_bfree  += (m->dfree * m->bsize) / sv->f_bsize;
    sv->f_bavail += (m->dfree * m->bsize) / sv->f_bsize;
  }
  delmembers(km);
  lprintf(0, "[debug] %s: EXIT path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_flush(const char *path, struct fuse_file_info *fi)
{
  ktask kt;
  lprintf(0, "[debug] %s: CALL path=%s\n", __func__, path);
  lprintf(0, "[debug] %s: EXIT path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_fsync(const char *path, int sync, struct fuse_file_info *fi)
{
  ktask kt;
  lprintf(0, "[debug] %s: CALL path=%s sync=%d\n", __func__, path, sync);
  lprintf(0, "[debug] %s: EXIT path=%s sync=%d\n", __func__, path, sync);
  return(0);
}

static int mtnmount_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
  ktask kt;
  lprintf(0, "%s: path=%s flags=%d\n", __func__, path, flags);
  return(0);
}

static int mtnmount_getxattr(const char *path, const char *name, char *value, size_t size)
{
  ktask kt;
  lprintf(0, "%s: path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_listxattr(const char *path, char *list, size_t size)
{
  ktask kt;
  lprintf(0, "%s: path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_removexattr(const char *path, const char *name)
{
  ktask kt;
  lprintf(0, "%s: path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_opendir(const char *path, struct fuse_file_info *fi)
{
  ktask kt;
  lprintf(0, "[debug] %s: CALL path=%s\n", __func__, path);
  lprintf(0, "[debug] %s: EXIT path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_releasedir(const char *path, struct fuse_file_info *fi)
{
  ktask kt;
  lprintf(0, "[debug] %s: path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_fsyncdir (const char *path, int flags, struct fuse_file_info *fi)
{
  ktask kt;
  lprintf(0, "%s: path=%s\n", __func__, path);
  return(0);
}

static void *mtnmount_init(struct fuse_conn_info *conn)
{
  lprintf(0, "[debug] %s:\n", __func__);
  return(NULL);
}

static void mtnmount_destroy(void *buff)
{
  lprintf(0, "[debug] %s:\n", __func__);
}

static int mtnmount_access (const char *path, int mode)
{
  ktask kt;
  lprintf(0, "%s: path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi)
{
  ktask kt;
  lprintf(0, "[debug] %s: CALL path=%s\n", __func__, path);
  lprintf(0, "[debug] %s: EXIT path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_lock(const char *path, struct fuse_file_info *fi, int cmd, struct flock *l)
{
  ktask kt;
  lprintf(0, "%s: path=%s cmd=%d\n", __func__, path, cmd);
  return(0);
}

static int mtnmount_utimens(const char *path, const struct timespec tv[2])
{
  ktask kt;
  lprintf(0, "[debug] %s: CALL\n", __func__);
  lprintf(0, "[debug] %s: EXIT\n",   __func__);
  return(0);
}

static int mtnmount_bmap(const char *path, size_t blocksize, uint64_t *idx)
{
  ktask kt;
  lprintf(0, "%s: path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_ioctl(const char *path, int cmd, void *arg, struct fuse_file_info *fi, unsigned int flags, void *data)
{
  ktask kt;
  lprintf(0, "%s: path=%s\n", __func__, path);
  return(0);
}

static int mtnmount_poll(const char *path, struct fuse_file_info *fi, struct fuse_pollhandle *ph, unsigned *reventsp)
{
  ktask kt;
  lprintf(0, "%s: path=%s\n", __func__, path);
  return(0);
}

static struct fuse_operations mtn_oper = {
  .getattr     = mtnmount_getattr,
  .readlink    = mtnmount_readlink,
  .readdir     = mtnmount_readdir,
  .create      = mtnmount_create,
  .open        = mtnmount_open,
  .read        = mtnmount_read,
  .write       = mtnmount_write,
  .release     = mtnmount_release,
  .unlink      = mtnmount_unlink,
  .rmdir       = mtnmount_rmdir,
  .mkdir       = mtnmount_mkdir,
  .rename      = mtnmount_rename,
  .chmod       = mtnmount_chmod,
  .chown       = mtnmount_chown,
  .truncate    = mtnmount_truncate,
  .statfs      = mtnmount_statfs,
  .opendir     = mtnmount_opendir,
  .releasedir  = mtnmount_releasedir,
  .fsyncdir    = mtnmount_fsyncdir,
  .init        = mtnmount_init,
  .destroy     = mtnmount_destroy,
  .ftruncate   = mtnmount_ftruncate,
  .fgetattr    = mtnmount_fgetattr,
  .utimens     = mtnmount_utimens,
  //.access      = mtnmount_access,
  //.flush       = mtnmount_flush,
  //.fsync       = mtnmount_fsync,
  //.setxattr    = mtnmount_setxattr,
  //.getxattr    = mtnmount_getxattr,
  //.listxattr   = mtnmount_listxattr,
  //.removexattr = mtnmount_removexattr,
  //.lock        = mtnmount_lock,
  //.bmap        = mtnmount_bmap,
  //.ioctl       = mtnmount_ioctl,
  //.poll        = mtnmount_poll,
};

int main(int argc, char *argv[])
{
  int i;
  mtn_init_option();
  mtn_rmutex = malloc(sizeof(pthread_mutex_t) * MTN_OPENLIMIT);
  mtn_wmutex = malloc(sizeof(pthread_mutex_t) * MTN_OPENLIMIT);
  for(i=0;i<MTN_OPENLIMIT;i++){
    pthread_mutex_init(&(mtn_rmutex[i]), NULL);
    pthread_mutex_init(&(mtn_wmutex[i]), NULL);
  }
  return fuse_main(argc, argv, &mtn_oper, NULL);
}

