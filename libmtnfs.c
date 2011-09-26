/*
 * libmtnfs.c
 * Copyright (C) 2011 KLab Inc.
 */
#include "mtnfs.h"
#include "common.h"
#include "libmtn.h"
int dcount = 0;
int scount = 0;
int mcount = 0;
pthread_mutex_t sqno_mutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t debug_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cache_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t count_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t member_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;

int is_loop = 1;
koption kopt;
char *mtncmdstr[]={
  "STARTUP",
  "SHUTDOWN",
  "HELLO",
  "INFO",
  "STAT",
  "LIST",
  "SET",
  "GET",
  "DEL",
  "DATA",
  "OPEN",
  "READ",
  "WRITE",
  "CLOSE",
  "TRUNCATE",
  "MKDIR",
  "RMDIR",
  "UNLINK",
  "RENAME",
  "CHMOD",
  "CHOWN",
  "GETATTR",
  "SETATTR",
  "SYMLINK",
  "READLINK",
  "UTIME",
  "RESULT",
};

void mtn_init(const char *name)
{
  FILE *fp;
  char buff[256];
  memset((void *)&kopt, 0, sizeof(kopt));
  sprintf(kopt.mtnstatus_name, ".mtnstatus");
  sprintf(kopt.mtnstatus_path, "/.mtnstatus");
  sprintf(kopt.mcast_addr,     "224.1.0.110");
  sprintf(kopt.module_name, name ? name : "");
  kopt.mcast_port = 6000;
  kopt.daemonize  = 1;
  kopt.free_limit = atoikmg("2G");
  kopt.max_packet_size = 1024;
  getcwd(kopt.cwd, PATH_MAX);
  gethostname(kopt.host, sizeof(kopt.host));
  fp = fopen("/proc/sys/net/core/rmem_max", "r");
  if(fread(buff, 1, sizeof(buff), fp) > 0){
    kopt.rcvbuf = atoi(buff);
  }
  fclose(fp);
}

int get_debuglevel()
{
  int d;
  pthread_mutex_lock(&(debug_mutex));
  d = kopt.debuglevel;
  pthread_mutex_unlock(&(debug_mutex));
  return(d);
}

void set_debuglevel(int d)
{
  pthread_mutex_lock(&(debug_mutex));
  kopt.debuglevel = d;
  pthread_mutex_unlock(&(debug_mutex));
}

uint16_t sqno()
{
  uint16_t r;
  static uint16_t sqno = 0;
  pthread_mutex_lock(&sqno_mutex);
  r = ++sqno;
  pthread_mutex_unlock(&sqno_mutex);
  return(r);
}

int cmp_addr(kaddr *a1, kaddr *a2)
{
  void *p1 = &(a1->addr.in.sin_addr);
  void *p2 = &(a2->addr.in.sin_addr);
  size_t size = sizeof(a1->addr.in.sin_addr);
  if(memcmp(p1, p2, size) != 0){
    return(1);
  }
  if(a1->addr.in.sin_port != a2->addr.in.sin_port){
    return(1);
  }
  return(0);
}

int cmp_task(ktask *t1, ktask *t2)
{
  if(cmp_addr(&(t1->addr), &(t2->addr)) != 0){
    return(1);
  }
  if(t1->recv.head.sqno != t2->recv.head.sqno){
    return(1);
  }
  return(0);
}

int send_readywait(int s)
{
  int r;
  int e = epoll_create(1);
  struct epoll_event ev;
  ev.data.fd = s;
  ev.events  = EPOLLOUT;
  if(e == -1){
    lprintf(0, "[error] %s: epoll_create %s\n", __func__, strerror(errno));
    return(0);
  }
  if(epoll_ctl(e, EPOLL_CTL_ADD, s, &ev) == -1){
    close(e);
    lprintf(0, "[error] %s: epoll_ctl %s\n", __func__, strerror(errno));
    return(0);
  }
  do{
    r = epoll_wait(e, &ev, 1, 1000);
    if(r == 1){
      close(e);
      return(1);
    }
    if(r == -1){
      if(errno == EINTR){
        continue;
      }
      lprintf(0,"[error] %s: %s\n", __func__, strerror(errno));
      break;
    }
  }while(is_loop);
  close(e);
  return(0);
}

int recv_readywait(int s)
{
  int r;
  int e = epoll_create(1);
  struct epoll_event ev;
  ev.data.fd = s;
  ev.events  = EPOLLIN;
  if(e == -1){
    lprintf(0, "[error] %s: epoll_create %s\n", __func__, strerror(errno));
    return(0);
  }
  if(epoll_ctl(e, EPOLL_CTL_ADD, s, &ev) == -1){
    lprintf(0, "[error] %s: epoll_ctl %s\n", __func__, strerror(errno));
    return(0);
  }
  while(is_loop){
    r = epoll_wait(e, &ev, 1, 1000);
    if(r == 1){
      close(e);
      return(1);
    }
    if(r == -1){
      if(errno == EINTR){
        continue;
      }
      lprintf(0,"[error] %s: %s\n", __func__, strerror(errno));
      break;
    }
  }
  close(e);
  return(0);
}

int recv_dgram(int s, kdata *data, struct sockaddr *addr, socklen_t *alen)
{
  int r;
  while(is_loop){
    r = recvfrom(s, data, sizeof(kdata), 0, addr, alen);
    if(r >= 0){
      break;
    }
    if(errno == EAGAIN){
      return(-1);
    }
    if(errno == EINTR){
      continue;
    }
    lprintf(0, "[error] %s: %s recv error\n", __func__, strerror(errno));
    return(-1);
  }
  if(r < sizeof(khead)){
    lprintf(0, "[error] %s: head size error\n", __func__);
    return(-1);
  }
  if(data->head.ver != PROTOCOL_VERSION){
    lprintf(0, "[error] %s: protocol error %d != %d\n", __func__, data->head.ver, PROTOCOL_VERSION);
    return(-1);
  }
  data->head.size = ntohs(data->head.size);
  if(r != data->head.size + sizeof(khead)){
    lprintf(0, "[error] %s: data size error\n", __func__);
    return(-1);
  }  
  data->head.sqno = ntohs(data->head.sqno);
  return(0);
}

int recv_stream(int s, void *buff, size_t size)
{
  int r;
  while(size){
    if(recv_readywait(s) == 0){
      return(1);
    }
    r = read(s, buff, size);
    if(r == -1){
      if(errno == EAGAIN){
        lprintf(0, "[warn] %s: %s\n", __func__, strerror(errno));
        continue;
      }
      if(errno == EINTR){
        lprintf(0, "[warn] %s: %s\n", __func__, strerror(errno));
        continue;
      }else{
        lprintf(0, "[error] %s: %s\n", __func__, strerror(errno));
        return(-1);
      }
    }
    if(r == 0){
      return(1);
    }
    buff += r;
    size -= r;
  }
  return(0);
}

int recv_data_stream(int s, kdata *kd)
{
  int r;
  if(r = recv_stream(s, &(kd->head), sizeof(kd->head))){
    return(r);
  }
  if(kd->head.size > MAX_DATASIZE){
    lprintf(0, "[debug] %s: size=%d\n", __func__, kd->head.size);
    return(-1);
  }
  return(recv_stream(s, &(kd->data), kd->head.size));
}

int send_dgram(int s, kdata *data, kaddr *addr)
{
  kdata sd;
  int size = data->head.size + sizeof(khead);
  memcpy(&sd, data, size);
  sd.head.ver  = PROTOCOL_VERSION;
  sd.head.size = htons(data->head.size);
  sd.head.sqno = htons(data->head.sqno);
  while(send_readywait(s)){
    int r = sendto(s, &sd, size, 0, &(addr->addr.addr), addr->len);
    if(r == size){
      return(0); /* success */
    }
    if(r == -1){
      if(errno == EAGAIN){
        continue;
      }
      if(errno == EINTR){
        continue;
      }
    }
    break;
  }
  return(-1);
}

int send_stream(int s, char *buff, size_t size)
{
  int r;
  while(send_readywait(s)){
    r = send(s, buff, size, 0);
    if(r == -1){
      if(errno == EINTR){
        continue;
      }
      return(-1);
    }
    if(size == r){
      break;
    }
    size -= r;
    buff += r;
  }
  return(0);
}

int send_data_stream(int s, kdata *data)
{
  size_t   size;
  uint8_t *buff;
  size  = sizeof(khead);
  size += data->head.size;
  buff  = (uint8_t *)data;
  return(send_stream(s, buff, size));
}

int send_recv_stream(int s, kdata *sd, kdata *rd)
{
  if(send_data_stream(s, sd) == -1){
    lprintf(0, "[error] %s: send error %s\n", __func__, strerror(errno));
    return(-1);
  }
  if(recv_data_stream(s, rd) == -1){
    lprintf(0, "[error] %s: recv error %s\n", __func__, strerror(errno));
    return(-1);
  }
  return(0);
}

int create_socket(int port, int mode)
{
  int s;
  int reuse = 1;
  struct sockaddr_in addr;
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;
  s = socket(AF_INET, mode, 0);
  if(s == -1){
    lprintf(0, "[error] %s: can't create socket\n", __func__);
    return(-1);
  }
  if(kopt.rcvbuf){
    if(setsockopt(s, SOL_SOCKET, SO_RCVBUF, (void *)&(kopt.rcvbuf), sizeof(kopt.rcvbuf)) == -1){
      lprintf(0, "[error] %s: %s setsockopt SO_RCVBUF error\n", __func__, strerror(errno));
    }
  }
  if(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(reuse)) == -1){
    lprintf(0, "[error] %s: SO_REUSEADDR error\n", __func__);
    return(-1);
  }
  if(bind(s, (struct sockaddr*)&addr, sizeof(addr)) == -1){
    lprintf(0, "[error] %s: bind error\n", __func__);
    return(-1);
  }
  return(s);
}

int create_usocket()
{
  int s = create_socket(0, SOCK_DGRAM);
  if(s == -1){
    return(-1);
  }
  if(fcntl(s, F_SETFL , O_NONBLOCK)){
    lprintf(0, "[error] %s: O_NONBLOCK %s\n", __func__, strerror(errno));
    return(-1);
  }
  return(s);
}

int create_lsocket(int port)
{
  int s = create_socket(port, SOCK_STREAM);
  if(s == -1){
    return(-1);
  }
  return(s);
}


int create_msocket(int port)
{
  char lpen = 1;
  char mttl = 1;
  struct ip_mreq mg;
  mg.imr_multiaddr.s_addr = inet_addr(kopt.mcast_addr);
  mg.imr_interface.s_addr = INADDR_ANY;

  int s = create_socket(port, SOCK_DGRAM);
  if(s == -1){
    return(-1);
  }
  if(fcntl(s, F_SETFL , O_NONBLOCK)){
    close(s);
    lprintf(0, "[error] %s: O_NONBLOCK %s\n", __func__, strerror(errno));
    return(-1);
  }
  if(setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void *)&mg, sizeof(mg)) == -1){
    lprintf(0, "[error] %s: IP_ADD_MEMBERSHIP error\n", __func__);
    close(s);
    return(-1);
  }
  if(setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,   (void *)&mg.imr_interface.s_addr, sizeof(mg.imr_interface.s_addr)) == -1){
    close(s);
    lprintf(0, "[error] %s: IP_MULTICAST_IF error\n", __func__);
    return(-1);
  }
  if(setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (void *)&lpen, sizeof(lpen)) == -1){
    close(s);
    lprintf(0, "[error] %s: IP_MULTICAST_LOOP error\n", __func__);
    return(-1);
  }
  if(setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL,  (void *)&mttl, sizeof(mttl)) == -1){
    close(s);
    lprintf(0, "[error] %s: IP_MULTICAST_TTL error\n", __func__);
    return(-1);
  }
  return(s);
}

char *v4addr(kaddr *addr, char *buff, socklen_t size)
{
  if(!addr){
    strcpy(buff, "0.0.0.0");
    return(buff);
  }
  if(inet_ntop(AF_INET, &(addr->addr.in.sin_addr), buff, size) == NULL){
    strcpy(buff, "0.0.0.0");
  }
  return(buff);
}

int v4port(kaddr *addr)
{
  return(ntohs(addr->addr.in.sin_port));
}

int mtn_get_string(uint8_t *str, kdata *kd)
{
  uint16_t len;
  uint16_t size = kd->head.size;
  uint8_t *buff = kd->data.data;
  if(size > MAX_DATASIZE){
    return(-1);
  }
  for(len=0;len<size;len++){
    if(*(buff + len) == 0){
      break;
    }
  }
  if(len == size){
    return(0);
  }
  len++;
  size -= len;
  if(str){
    memcpy(str, buff, len);
    memmove(buff, buff + len, size);
    kd->head.size = size;
  }
  return(len);
}

int mtn_get_int16(uint16_t *val, kdata *kd)
{
  uint16_t len  = sizeof(uint16_t);
  uint16_t size = kd->head.size;
  if(size > MAX_DATASIZE){
    return(-1);
  }
  if(size < len){
    return(-1);
  }
  size -= len;
  *val = ntohs(kd->data.data16);
  if(kd->head.size = size){
    memmove(kd->data.data, kd->data.data + len, size);
  }
  return(0);
}

int mtn_get_int32(uint32_t *val, kdata *kd)
{
  uint16_t len  = sizeof(uint32_t);
  uint16_t size = kd->head.size;
  if(size > MAX_DATASIZE){
    lprintf(9, "[debug] %s: size=%d\n", __func__, size);
    return(-1);
  }
  if(size < len){
    lprintf(9, "[debug] %s: size=%d len=%d\n", __func__, size, len);
    return(-1);
  }
  size -= len;
  *val = ntohl(kd->data.data32);
  if(kd->head.size = size){
    memmove(kd->data.data, kd->data.data + len, size);
  }
  return(0);
}

int mtn_get_int64(uint64_t *val, kdata *kd)
{
  uint16_t  len = sizeof(uint64_t);
  uint16_t size = kd->head.size;
  if(size > MAX_DATASIZE){
    return(-1);
  }
  if(size < len){
    return(-1);
  }
  size -= len;
  uint32_t *ptr = (uint32_t *)(kd->data.data);
  uint64_t hval = (uint64_t)(ntohl(*(ptr + 0)));
  uint64_t lval = (uint64_t)(ntohl(*(ptr + 1)));
  *val = (hval << 32) | lval;
  if(kd->head.size = size){
    memmove(kd->data.data, kd->data.data + len, size);
  }
  return(0);
}

int mtn_get_int(void *val, kdata *kd, int size)
{
  switch(size){
    case 2:
      return mtn_get_int16(val, kd);
    case 4:
      return mtn_get_int32(val, kd);
    case 8:
      return mtn_get_int64(val, kd);
  }
  return(-1);
}

int mtn_set_string(uint8_t *str, kdata *kd)
{
  uint16_t len;
  if(str == NULL){
    *(kd->data.data + kd->head.size) = 0;
    kd->head.size++;
    return(0);
  }
  len = strlen(str) + 1;
  if(kd){
    if(kd->head.size + len > MAX_DATASIZE){
      return(-1);
    }
    memcpy(kd->data.data + kd->head.size, str, len);
    kd->head.size += len;
  }
  return(len);
}

int mtn_set_int16(uint16_t *val, kdata *kd)
{
  uint16_t len = sizeof(uint16_t);
  if(kd){
    if(kd->head.size + len > MAX_DATASIZE){
      return(-1);
    }
    *(uint16_t *)(kd->data.data + kd->head.size) = htons(*val);
    kd->head.size += len;
  }
  return(len);
}

int mtn_set_int32(uint32_t *val, kdata *kd)
{
  uint16_t len = sizeof(uint32_t);
  if(kd){
    if(kd->head.size + len > MAX_DATASIZE){
      return(-1);
    }
    *(uint32_t *)(kd->data.data + kd->head.size) = htonl(*val);
    kd->head.size += len;
  }
  return(len);
}

int mtn_set_int64(uint64_t *val, kdata *kd)
{
  uint16_t  len = sizeof(uint64_t);
  uint32_t hval = (*val) >> 32;
  uint32_t lval = (*val) & 0xFFFFFFFF;
  uint32_t *ptr = NULL;
  if(kd){
    ptr = (uint32_t *)(kd->data.data + kd->head.size);
    if(kd->head.size + len > MAX_DATASIZE){
      return(-1);
    }
    *(ptr + 0) = htonl(hval);
    *(ptr + 1) = htonl(lval);
    kd->head.size += len;
  }
  return(len);
}

int mtn_set_int(void *val, kdata *kd, int size)
{
  switch(size){
    case 2:
      return mtn_set_int16(val, kd);
    case 4:
      return mtn_set_int32(val, kd);
    case 8:
      return mtn_set_int64(val, kd);
  }
  return(-1);
}

int mtn_set_data(void *buff, kdata *kd, size_t size)
{
  size_t max = MAX_DATASIZE - kd->head.size;
  if(MAX_DATASIZE <= kd->head.size){
    return(0);
  }
  if(size > max){
    size = max;
  }
  memcpy(kd->data.data + kd->head.size, buff, size);
  kd->head.size += size;
  return(size);
}

int mtn_set_stat(struct stat *st, kdata *kd)
{
  int r = 0;
  int l = 0;
  if(st){
    r = mtn_set_int(&(st->st_mode),  kd, sizeof(st->st_mode));
    if(r == -1){return(-1);}else{l+=r;}

    r = mtn_set_int(&(st->st_size),  kd, sizeof(st->st_size));
    if(r == -1){return(-1);}else{l+=r;}

    r = mtn_set_int(&(st->st_uid),   kd, sizeof(st->st_uid));
    if(r == -1){return(-1);}else{l+=r;}

    r = mtn_set_int(&(st->st_gid),   kd, sizeof(st->st_gid));
    if(r == -1){return(-1);}else{l+=r;}

    r = mtn_set_int(&(st->st_blocks),kd, sizeof(st->st_blocks));
    if(r == -1){return(-1);}else{l+=r;}

    r = mtn_set_int(&(st->st_atime), kd, sizeof(st->st_atime));
    if(r == -1){return(-1);}else{l+=r;}

    r = mtn_set_int(&(st->st_mtime), kd, sizeof(st->st_mtime));
    if(r == -1){return(-1);}else{l+=r;}
  }
  return(l);
}

int mtn_get_stat(struct stat *st, kdata *kd)
{
  if(st && kd){
    if(mtn_get_int(&(st->st_mode),  kd, sizeof(st->st_mode)) == -1){
      lprintf(9, "[error] %s: 1\n", __func__);
      return(-1);
    }
    if(mtn_get_int(&(st->st_size),  kd, sizeof(st->st_size)) == -1){
      lprintf(9, "[error] %s: 2\n", __func__);
      return(-1);
    }
    if(mtn_get_int(&(st->st_uid),   kd, sizeof(st->st_uid)) == -1){
      lprintf(9, "[error] %s: 3\n", __func__);
      return(-1);
    }
    if(mtn_get_int(&(st->st_gid),   kd, sizeof(st->st_gid)) == -1){
      lprintf(9, "[error] %s: 4\n", __func__);
      return(-1);
    }
    if(mtn_get_int(&(st->st_blocks),kd, sizeof(st->st_blocks)) == -1){
      lprintf(9, "[error] %s: 5\n", __func__);
      return(-1);
    }
    if(mtn_get_int(&(st->st_atime), kd, sizeof(st->st_atime)) == -1){
      lprintf(9, "[error] %s: 6\n", __func__);
      return(-1);
    }
    if(mtn_get_int(&(st->st_mtime), kd, sizeof(st->st_mtime)) == -1){
      lprintf(9, "[error] %s: 7\n", __func__);
      return(-1);
    }
    st->st_nlink = S_ISDIR(st->st_mode) ? 2 : 1;
    return(0);
  }
  lprintf(9, "[error] %s: 8\n", __func__);
  return(-1);
}

void get_mode_string(uint8_t *buff, mode_t mode)
{
  uint8_t *perm[] = {"---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx"};
  if(S_ISREG(mode)){
    *(buff++) = '-';
  }else if(S_ISDIR(mode)){
    *(buff++) = 'd';
  }else if(S_ISCHR(mode)){
    *(buff++) = 'c';
  }else if(S_ISBLK(mode)){
    *(buff++) = 'b';
  }else if(S_ISFIFO(mode)){
    *(buff++) = 'p';
  }else if(S_ISLNK(mode)){
    *(buff++) = 'l';
  }else if(S_ISSOCK(mode)){
    *(buff++) = 's';
  }
  int m;
  m = (mode >> 6) & 7;
  strcpy(buff, perm[m]);
  buff += 3;
  m = (mode >> 3) & 7;
  strcpy(buff, perm[m]);
  buff += 3;
  m = (mode >> 0) & 7;
  strcpy(buff, perm[m]);
  buff += 3;
  *buff = 0;
}

int is_mtnstatus(const char *path, char *buff)
{
  int len = strlen(kopt.mtnstatus_path);
  if(strlen(path) < len){
    return(0);
  }
  if(memcmp(path, kopt.mtnstatus_path, len)){
    return(0);
  }
  if(path[len] == 0){
    if(buff){
      *buff = 0;
    }
  }else{
    if(path[len] != '/'){
      return(0);
    }
    if(buff){
      strcpy(buff, path + len + 1);
    }
  }
  return(1);
}

//----------------------------------------------------------------
// new
//----------------------------------------------------------------
kdir *newkdir(const char *path)
{
  kdir *kd;
  lprintf(9, "[debug] %s: path=%s\n", __func__, path);
  kd = xmalloc(sizeof(kdir));
  memset(kd,0,sizeof(kdir));
  strcpy(kd->path, path);
  kcount(1,0,0);
  return(kd);
}

kstat *newstat(const char *name)
{
  char b[PATH_MAX];
  char f[PATH_MAX];
  kstat *kst = xmalloc(sizeof(kstat));
  memset(kst, 0, sizeof(kstat));
  if(!name){
    b[0] = 0;
  }else{
    strcpy(b, name);
  }
  strcpy(f, basename(b));
  kst->name = xmalloc(strlen(f) + 1);
  strcpy(kst->name, f);
  kcount(0,1,0);
  return(kst);
}

kmember *newmember()
{
  kmember *km;
  km = xmalloc(sizeof(kmember));
  memset(km,0,sizeof(kmember));
  kcount(0,0,1);
  return(km);
}

//----------------------------------------------------------------
// del
//----------------------------------------------------------------
kdir *deldir(kdir *kd)
{
	kdir *n;
  if(!kd){
    return(NULL);
  }
  lprintf(9, "[debug] %s: path=%s\n", __func__, kd->path);
  delstats(kd->kst);
  kd->kst = NULL;
  if(kd->prev){
    kd->prev->next = kd->next;
  }
  if(kd->next){
    kd->next->prev = kd->prev;
  }
	n = kd->next;
  kd->prev = NULL;
  kd->next = NULL;
  xfree(kd);
  kcount(-1,0,0);
	return(n);
}

kmember *del_member(kmember *km)
{
  kmember *nm;
  if(km == NULL){
    return(NULL);
  }
  nm = km->next;
  if(km->host){
    xfree(km->host);
    km->host = NULL;
  }
  if(km->prev){
    km->prev->next = km->next;
  }
  if(km->next){
    km->next->prev = km->prev;
  }
  km->next = NULL;
  km->prev = NULL;
  xfree(km);
  kcount(0, 0, -1);
  return(nm);
}

void del_members(kmember *members)
{
  while(members){
    members = del_member(members);
  }
}

kstat *delstat(kstat *kst)
{
	kstat *r = NULL;
  if(!kst){
    return NULL;
  }
  if(kst->prev){
		kst->prev->next = kst->next;
	}
  if(kst->next){
		r = kst->next;
		kst->next->prev = kst->prev;
  }
  kst->prev = NULL;
  kst->next = NULL;
  if(kst->name){
    xfree(kst->name);
    kst->name = NULL;
  }
  del_members(kst->member);
  kst->member = NULL;
  xfree(kst);
  kcount(0, -1, 0);
	return(r);
}

void delstats(kstat *kst)
{
	while(kst){
		kst = delstat(kst);
	}
}

//----------------------------------------------------------------
// 
//----------------------------------------------------------------
kmember *get_member(kmember *members, kaddr *addr)
{
  while(members){
    if(cmp_addr(&(members->addr), addr) == 0){
      return(members);
    }
    members = members->next;
  }
  return(NULL);
}

kmember *add_member(kmember *members, kaddr *addr, uint8_t *host)
{
  kmember *mb = get_member(members, addr);
  if(mb == NULL){
    mb = newmember();
    memcpy(&(mb->addr), addr, sizeof(kaddr));
    if(mb->next = members){
      members->prev = mb;
    }
    members = mb;
  }
  if(host){
    int len = strlen(host) + 1;
    mb->host = xrealloc(mb->host, len);
    memcpy(mb->host, host, len);
  }
  mb->mark = 0;
  return(members);
}

kmember *copy_member(kmember *km)
{
  kmember *nkm;
  if(km == NULL){
    return(NULL);
  }
  nkm = add_member(NULL, &(km->addr), km->host);
  nkm->mark  = km->mark;
  nkm->bsize = km->bsize;
  nkm->fsize = km->fsize;
  nkm->dsize = km->dsize;
  nkm->dfree = km->dfree;
  nkm->vsz   = km->vsz;
  nkm->res   = km->res;
  memcpy(&(nkm->tv), &(km->tv), sizeof(struct timeval));
  return(nkm);
}

uint32_t get_members_count(kmember *mb)
{
  uint32_t mcount = 0;
  while(mb){
    mcount++;
    mb = mb->next;
  }
  return(mcount);
}

kmember *get_members(){
  kmember *mb;
  kmember *members;
  struct timeval tv;
  pthread_mutex_lock(&(member_mutex));
  gettimeofday(&tv, NULL);
  if((tv.tv_sec - kopt.member_tv.tv_sec) > 30){
    del_members(kopt.members);
    kopt.members = NULL;
  }
  if(kopt.members == NULL){
    if((tv.tv_sec - kopt.member_tv.tv_sec) > 20){
      kopt.members = mtn_hello();
      memcpy(&(kopt.member_tv), &tv, sizeof(struct timeval));
    }
  }
  members = NULL;
  for(mb=kopt.members;mb;mb=mb->next){
    members = add_member(members, &(mb->addr), mb->host);
  }
  pthread_mutex_unlock(&(member_mutex));
  return(members);
}

//-------------------------------------------------------------------
//
//-------------------------------------------------------------------
int mtn_process_hello(int s, kdata *sdata, kdata *rdata, kaddr *saddr, kaddr *raddr, MTNPROCFUNC mtn)
{
  mtn(NULL, sdata, rdata, raddr);
  sdata->head.flag = 1;
  send_dgram(s, sdata, raddr);
  sdata->head.flag = 0;
  return(sdata->opt32 > get_members_count(sdata->option));
}

int mtn_process_member(int s, kmember *members, kdata *sdata, kdata *rdata, kaddr *saddr, kaddr *raddr, MTNPROCFUNC mtn)
{
  int r = 1;
  kmember *mb;
  members = add_member(members, raddr, NULL);
  mb = get_member(members, raddr);
  mtn(mb, sdata, rdata, raddr);
  if(rdata->head.fin){
    mb->mark = 1;
    for(mb=members;mb && mb->mark;mb=mb->next);
    r = (mb == NULL) ? 0 : 1;
  }
  return(r);
}

int mtn_process_recv(int s, kmember *members, kdata *sdata, kaddr *saddr, MTNPROCFUNC mtn)
{
  int   r = 1;
  kaddr raddr;
  kdata rdata;
  memset(&raddr, 0, sizeof(raddr));
  raddr.len = sizeof(raddr.addr);
  while(recv_dgram(s, &rdata, &(raddr.addr.addr), &(raddr.len)) == 0){
    if(members == NULL){
      r = mtn_process_hello(s, sdata, &rdata, saddr, &raddr, mtn);
    }else{
      r = mtn_process_member(s, members, sdata, &rdata, saddr, &raddr, mtn);
    }
  }
  return(r);
}

void mtn_process_wait(int s, kmember *members, kdata *sdata, kaddr *saddr, MTNPROCFUNC mtn)
{
  kmember *mb;
  if(members == NULL){
    send_dgram(s, sdata, saddr);
  }else{
    for(mb=members;mb;mb=mb->next){
      if(mb->mark == 0){
        send_dgram(s, sdata, &(mb->addr));
      }
    }
  }
}

void mtn_process_loop(int s, kmember *members, kdata *sdata, kaddr *saddr, MTNPROCFUNC mtn)
{
  int r;
  int e;
  int l = 1;
  int o = 2000;
  int t = (members == NULL) ? 200 : 3000;
  struct epoll_event ev;
  if(mtn == NULL){
    return;
  }
  e = epoll_create(1);
  if(e == -1){
    lprintf(0, "[error] %s: %s\n", __func__, strerror(errno));
    return;
  }
  ev.data.fd = s;
  ev.events  = EPOLLIN;
  if(epoll_ctl(e, EPOLL_CTL_ADD, s, &ev) == -1){
    close(e);
    lprintf(0, "[error] %s: %s\n", __func__, strerror(errno));
    return;
  }
  while(is_loop && l){
    r = epoll_wait(e, &ev, 1, t);
    t = 200;
    switch(r){
      case -1:
        lprintf(0, "[error] %s: epoll %s\n", __func__, strerror(errno));
        break;
      case 0:
        if(l = ((o -= t) > 0)){
          mtn_process_wait(s, members, sdata, saddr, mtn);
        }
        break;
      default:
        l = mtn_process_recv(s, members, sdata, saddr, mtn);
        break;
    }
  }
  close(e);
}

void mtn_process(kmember *members, kdata *sdata, MTNPROCFUNC mtn)
{
  int s;
  kaddr saddr;
  if((members == NULL) && (mtn != NULL ) && (sdata->head.type != MTNCMD_HELLO)){
    return;
  }
  s = create_usocket();
  if(s == -1){
    lprintf(0, "[error] %s: %s\n", __func__, strerror(errno));
    return;
  }
  saddr.len                     = sizeof(struct sockaddr_in);
  saddr.addr.in.sin_family      = AF_INET;
  saddr.addr.in.sin_port        = htons(kopt.mcast_port);
  saddr.addr.in.sin_addr.s_addr = inet_addr(kopt.mcast_addr);
  sdata->head.ver               = PROTOCOL_VERSION;
  sdata->head.sqno              = sqno();
  if(send_dgram(s, sdata, &saddr) == -1){
    lprintf(0, "[error] %s: %s\n", __func__, strerror(errno));
  }else{
    mtn_process_loop(s, members, sdata, &saddr, mtn);
  }
  close(s);
}

void mtn_startup(int f)
{
  kdata data;
  lprintf(9,"[debug] %s: IN\n", __func__);
  lprintf(8,"[debug] %s: flag=%d\n", __func__, f);
  data.head.type = MTNCMD_STARTUP;
  data.head.size = 0;
  data.head.flag = f;
  data.option = NULL;
  mtn_set_string(kopt.host, &data);
  mtn_process(NULL, &data, NULL);
  lprintf(9,"[debug] %s: OUT\n", __func__);
}

void mtn_shutdown()
{
  kdata data;
  lprintf(9,"[debug] %s: IN\n", __func__);
  data.head.type = MTNCMD_SHUTDOWN;
  data.head.size = 0;
  data.head.flag = 1;
  data.option = NULL;
  mtn_set_string(kopt.host, &data);
  mtn_process(NULL, &data, NULL);
  lprintf(9,"[debug] %s: OUT\n", __func__);
}

void mtn_hello_process(kmember *member, kdata *sdata, kdata *rdata, kaddr *addr)
{
  uint32_t mcount;
  uint8_t host[1024];
  kmember *members = (kmember *)(sdata->option);
  if(mtn_get_string(host, rdata) == -1){
    lprintf(0, "%s: mtn get error\n", __func__);
    return;
  }
  mtn_get_int(&mcount, rdata, sizeof(mcount));
  members = add_member(members, addr, host);
  sdata->option = members;
  if(sdata->opt32 < mcount){
    sdata->opt32 = mcount;
  }
}

kmember *mtn_hello()
{
  kdata data;
  data.head.type = MTNCMD_HELLO;
  data.head.size = 0;
  data.head.flag = 0;
  data.option = NULL;
  data.opt32  = 0;
  lprintf(9, "[debug] %s: IN\n", __func__);
  mtn_process(NULL, &data, (MTNPROCFUNC)mtn_hello_process);
  lprintf(9, "[debug] %s: OUT\n", __func__);
  return((kmember *)(data.option));
}

void mtn_info_process(kmember *member, kdata *sdata, kdata *rdata, kaddr *addr)
{
  lprintf(9, "[debug] %s: IN\n", __func__);
  mtn_get_int(&(member->bsize),     rdata, sizeof(member->bsize));
  mtn_get_int(&(member->fsize),     rdata, sizeof(member->fsize));
  mtn_get_int(&(member->dsize),     rdata, sizeof(member->dsize));
  mtn_get_int(&(member->dfree),     rdata, sizeof(member->dfree));
  mtn_get_int(&(member->limit),     rdata, sizeof(member->limit));
  mtn_get_int(&(member->malloccnt), rdata, sizeof(member->malloccnt));
  mtn_get_int(&(member->membercnt), rdata, sizeof(member->membercnt));
  mtn_get_int(&(member->vsz),       rdata, sizeof(member->vsz));
  mtn_get_int(&(member->res),       rdata, sizeof(member->res));
  lprintf(9, "[debug] %s: OUT\n", __func__);
}

kmember *mtn_info()
{
  lprintf(9, "[debug] %s: IN\n", __func__);
  kdata data;
  kmember *mb;
  kmember *members = get_members();
  data.head.type   = MTNCMD_INFO;
  data.head.size   = 0;
  data.head.flag   = 0;
  mtn_process(members, &data, (MTNPROCFUNC)mtn_info_process);
  for(mb=members;mb;mb=mb->next){
    mb->mark = 0;
  }
  lprintf(9, "[debug] %s: OUT\n", __func__);
  return(members);
}

kstat *mkstat(kmember *member, kaddr *addr, kdata *data)
{
  kstat *kst = NULL;
  int len = mtn_get_string(NULL, data);
  if(len == -1){
    lprintf(0,"[error] %s: data error\n", __func__);
    return(NULL);
  }
  if(len == 0){
    return(NULL);
  }
  kst = newstat(NULL);
  kst->name = xrealloc(kst->name, len);
  mtn_get_string(kst->name,  data);
  mtn_get_stat(&(kst->stat), data);
  kst->member = copy_member(member);
  lprintf(9, "[debug] %s: name=%s\n", __func__, kst->name);
  if(kst->next = mkstat(member, addr, data)){
    kst->next->prev = kst;
  }
  return kst;
}

kstat *mgstat(kstat *krt, kstat *kst)
{
  kstat *st;
  kstat *rt;
  if(!krt){
    return(kst);
  }
  rt = krt;
  while(rt){
    st = kst;
    while(st){
      if(strcmp(rt->name, st->name) == 0){
        if(rt->stat.st_mtime < st->stat.st_mtime){
          memcpy(&(rt->stat), &(st->stat), sizeof(struct stat));
          del_members(rt->member);
          rt->member = st->member;
          st->member = NULL;
        }
        if(st == kst){
          kst = (st = delstat(st));
        }else{
          st = delstat(st);
        }
        continue;
      }
      st = st->next;
    }
    rt = rt->next;
  }
  if(kst){
    for(rt=krt;rt->next;rt=rt->next);
    if(rt->next = kst){
      kst->prev = rt;
    }
  }
  return(krt);
}

kstat *copy_stats(kstat *kst)
{
  kstat *ks = NULL;
  kstat *kr = NULL;
  lprintf(9, "[debug] %s: IN\n", __func__);
  while(kst){
    ks = newstat(kst->name);
    memcpy(&(ks->stat), &(kst->stat), sizeof(struct stat));
    ks->member = copy_member(kst->member);
    if(ks->next = kr){
      kr->prev = ks;
    }
    kr = ks;
    kst=kst->next;
  }
  lprintf(9, "[debug] %s: OUT\n", __func__);
  return(kr);
}

void addstat_dircache(const char *path, kstat *kst)
{
  kdir *kd;
  if(kst == NULL){
    return;
  }
  pthread_mutex_lock(&(cache_mutex));
  kd = kopt.dircache;
  while(kd){
    if(strcmp(kd->path, path) == 0){
      break;
    }
    kd = kd->next;
  }
  if(kd == NULL){
    kd = newkdir(path);
    if(kd->next = kopt.dircache){
      kopt.dircache->prev = kd;
    }
    kopt.dircache = kd;
  }
  kd->kst = mgstat(kd->kst, kst);
  gettimeofday(&(kd->tv), NULL);
  pthread_mutex_unlock(&(cache_mutex));
}

void setstat_dircache(const char *path, kstat *kst)
{
  kdir *kd;
  pthread_mutex_lock(&(cache_mutex));
  kd = kopt.dircache;
  while(kd){
    if(strcmp(kd->path, path) == 0){
      break;
    }
    kd = kd->next;
  }
  if(kd == NULL){
    kd = newkdir(path);
    if(kd->next = kopt.dircache){
      kopt.dircache->prev = kd;
    }
    kopt.dircache = kd;
  }
  delstats(kd->kst);
  kd->kst  = copy_stats(kst);
  kd->flag = (kst == NULL) ? 0 : 1;
  gettimeofday(&(kd->tv), NULL);
  pthread_mutex_unlock(&(cache_mutex));
}

kstat *get_dircache(const char *path, int flag)
{
  kdir  *kd;
  kstat *kr;
  struct timeval tv;
  pthread_mutex_lock(&(cache_mutex));
  gettimeofday(&tv, NULL);
  kd = kopt.dircache;
  while(kd){
    if((tv.tv_sec - kd->tv.tv_sec) > 10){
      delstats(kd->kst);
      kd->kst  = NULL;
      kd->flag = 0;
    }
    if((tv.tv_sec - kd->tv.tv_sec) > 60){
      if(kd == kopt.dircache){
        kd = (kopt.dircache = deldir(kopt.dircache));
      }else{
        kd = deldir(kd);
      }
      continue;
    }
    if(strcmp(kd->path, path) == 0){
      break;
    }
    kd = kd->next;
  }
  if(kd == NULL){
    kd = newkdir(path);
    if(kd->next = kopt.dircache){
      kopt.dircache->prev = kd;
    }
    kopt.dircache = kd;
  }
  kr = (!flag || kd->flag) ? copy_stats(kd->kst) : NULL;
  pthread_mutex_unlock(&(cache_mutex));
  return(kr);
}

//----------------------------------------------------------------------------
//
// UDP
//
//----------------------------------------------------------------------------
void mtn_list_process(kmember *member, kdata *sdata, kdata *rdata, kaddr *addr)
{
  sdata->option = mgstat(sdata->option, mkstat(member, addr, rdata));
}

kstat *mtn_list(const char *path)
{
	lprintf(8, "[debug] %s:\n", __func__);
  kdata data;
  kmember *members = get_members();
  data.head.type   = MTNCMD_LIST;
  data.head.size   = 0;
  data.head.flag   = 0;
  data.option      = NULL;
  mtn_set_string((uint8_t *)path, &data);
  mtn_process(members, &data, (MTNPROCFUNC)mtn_list_process);
  del_members(members);
  return(data.option);
}

void mtn_stat_process(kmember *member, kdata *sd, kdata *rd, kaddr *addr)
{
  kstat *krt = sd->option;
	if(rd->head.type == MTNCMD_SUCCESS){
		kstat *kst = mkstat(member, addr, rd);
		sd->option = mgstat(krt, kst);
	}
}

kstat *mtn_stat(const char *path)
{
	lprintf(8, "[debug] %s:\n", __func__);
  kdata sd;
  kmember *members = get_members();
	memset(&sd, 0, sizeof(sd));
  sd.head.type = MTNCMD_STAT;
  sd.head.size = 0;
  sd.head.flag = 0;
  sd.option    = NULL;
  mtn_set_string((uint8_t *)path, &sd);
  mtn_process(members, &sd, (MTNPROCFUNC)mtn_stat_process);
  del_members(members);
  return(sd.option);
}

void mtn_choose_info(kmember *member, kdata *sdata, kdata *rdata, kaddr *addr)
{
  kmember *choose = sdata->option;
  mtn_get_int(&(member->bsize), rdata, sizeof(member->bsize));
  mtn_get_int(&(member->fsize), rdata, sizeof(member->fsize));
  mtn_get_int(&(member->dsize), rdata, sizeof(member->dsize));
  mtn_get_int(&(member->dfree), rdata, sizeof(member->dfree));
  mtn_get_int(&(member->vsz),   rdata, sizeof(member->vsz));
  mtn_get_int(&(member->res),   rdata, sizeof(member->res));
  if(choose == NULL){
    sdata->option = member;
  }else{
    if((member->dfree * member->bsize) > (choose->dfree * choose->bsize)){
      sdata->option = member;
    }
  }
}

void mtn_choose_list(kdata *sdata, kdata *rdata, kaddr *addr)
{
}

kmember *mtn_choose(const char *path)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kdata data;
  kmember *member;
  kmember *members = get_members();
  data.head.type = MTNCMD_INFO;
  data.head.size = 0;
  data.head.flag = 0;
  data.option    = NULL;
  mtn_process(members, &data, (MTNPROCFUNC)mtn_choose_info);
  member = copy_member(data.option);
  del_members(members);
  return(member);
}

void mtn_find_process(kmember *member, kdata *sdata, kdata *rdata, kaddr *addr)
{
	lprintf(9, "[debug] %s: IN\n", __func__);
	lprintf(9, "[debug] %s: P=%p  host=%s\n", __func__, sdata->option, member->host);
  sdata->option = mgstat(sdata->option, mkstat(member, addr, rdata));
	lprintf(9, "[debug] %s: P=%p\n", __func__, sdata->option);
	lprintf(9, "[debug] %s: OUT\n", __func__);
}

kstat *mtn_find(const char *path, int create_flag)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kstat *kst;
  kdata data;
  kmember *member;
  kmember *members = mtn_info();
  data.option      = NULL;
  data.head.type   = MTNCMD_LIST;
  data.head.size   = 0;
  data.head.flag   = 0;
  mtn_set_string((uint8_t *)path, &data);
  mtn_process(members, &data, (MTNPROCFUNC)mtn_find_process);
  kst = data.option;
  if(kst == NULL){
    if(create_flag){
      if(member = mtn_choose(path)){
        kst = newstat(path);
        kst->member = member;
      }
    }
  }
  del_members(members);
  return(kst);
}

void mtn_mkdir_process(kmember *member, kdata *sd, kdata *rd, kaddr *addr)
{
	if(rd->head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, rd, sizeof(errno));
	  lprintf(0,"[error] %s: %s %s\n", __func__, member->host, strerror(errno));
	}
}

int mtn_mkdir(const char *path)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kdata sd;
  kmember *members = get_members();
	memset(&sd, 0, sizeof(sd));
  if(strlen(path) > strlen(kopt.mtnstatus_path)){
    if(memcmp(kopt.mtnstatus_path, path, strlen(kopt.mtnstatus_path)) == 0){
      return(-EACCES);
    }
  }
  sd.head.type = MTNCMD_MKDIR;
  sd.head.size = 0;
  sd.head.flag = 0;
  sd.option    = NULL;
  mtn_set_string((uint8_t *)path, &sd);
  mtn_process(members, &sd, (MTNPROCFUNC)mtn_mkdir_process);
  del_members(members);
  return(0);
}

void mtn_rm_process(kmember *member, kdata *sd, kdata *rd, kaddr *addr)
{
	if(rd->head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, rd, sizeof(errno));
	  lprintf(0,"[error] %s: %s %s\n", __func__, member->host, strerror(errno));
	}
}

int mtn_rm(const char *path)
{
	lprintf(2, "[debug] %s: path=%s\n", __func__, path);
  kdata sd;
  kmember *members = get_members();
	memset(&sd, 0, sizeof(sd));
  if(strlen(path) > strlen(kopt.mtnstatus_path)){
    if(memcmp(kopt.mtnstatus_path, path, strlen(kopt.mtnstatus_path)) == 0){
      return(-EACCES);
    }
  }
  sd.head.type = MTNCMD_UNLINK;
  sd.head.size = 0;
  sd.head.flag = 0;
  sd.option    = NULL;
  mtn_set_string((uint8_t *)path, &sd);
  mtn_process(members, &sd, (MTNPROCFUNC)mtn_rm_process);
  del_members(members);
  return(0);
}

void mtn_rename_process(kmember *member, kdata *sd, kdata *rd, kaddr *addr)
{
	if(rd->head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, rd, sizeof(errno));
	  lprintf(0,"[error] %s: %s %s\n", __func__, member->host, strerror(errno));
	}
}

int mtn_rename(const char *opath, const char *npath)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kdata sd;
  kmember *members = get_members();
	memset(&sd, 0, sizeof(sd));
  if(strlen(opath) > strlen(kopt.mtnstatus_path)){
    if(memcmp(kopt.mtnstatus_path, opath, strlen(kopt.mtnstatus_path)) == 0){
      return(-EACCES);
    }
  }
  if(strlen(npath) > strlen(kopt.mtnstatus_path)){
    if(memcmp(kopt.mtnstatus_path, npath, strlen(kopt.mtnstatus_path)) == 0){
      return(-EACCES);
    }
  }
  sd.head.type = MTNCMD_RENAME;
  sd.head.size = 0;
  sd.head.flag = 0;
  sd.option    = NULL;
  mtn_set_string((uint8_t *)opath, &sd);
  mtn_set_string((uint8_t *)npath, &sd);
  mtn_process(members, &sd, (MTNPROCFUNC)mtn_rename_process);
  del_members(members);
  return(0);
}

void mtn_symlink_process(kmember *member, kdata *sd, kdata *rd, kaddr *addr)
{
	if(rd->head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, rd, sizeof(errno));
	  lprintf(0,"[error] %s: %s %s\n", __func__, member->host, strerror(errno));
	}
}

int mtn_symlink(const char *oldpath, const char *newpath)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kdata sd;
  kmember *members = get_members();
	memset(&sd, 0, sizeof(sd));
  if(strlen(newpath) > strlen(kopt.mtnstatus_path)){
    if(memcmp(kopt.mtnstatus_path, newpath, strlen(kopt.mtnstatus_path)) == 0){
      return(-EACCES);
    }
  }
  sd.head.type = MTNCMD_SYMLINK;
  sd.head.size = 0;
  sd.head.flag = 0;
  sd.option    = NULL;
  mtn_set_string((uint8_t *)oldpath, &sd);
  mtn_set_string((uint8_t *)newpath, &sd);
  mtn_process(members, &sd, (MTNPROCFUNC)mtn_symlink_process);
  del_members(members);
  return(0);
}

void mtn_readlink_process(kmember *member, kdata *sd, kdata *rd, kaddr *addr)
{
  size_t size;
	if(rd->head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, rd, sizeof(errno));
	  lprintf(0,"[error] %s: %s %s\n", __func__, member->host, strerror(errno));
	}else{
    if(sd->option == NULL){
      size = mtn_get_string(NULL, rd);
      sd->option = xmalloc(size);
      mtn_get_string(sd->option, rd);
    }
  }
}

int mtn_readlink(const char *path, char *buff, size_t size)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kdata sd;
  kmember *members = get_members();
	memset(&sd, 0, sizeof(sd));
  if(strlen(path) > strlen(kopt.mtnstatus_path)){
    if(memcmp(kopt.mtnstatus_path, path, strlen(kopt.mtnstatus_path)) == 0){
      return(-EACCES);
    }
  }
  sd.head.type = MTNCMD_READLINK;
  sd.head.size = 0;
  sd.head.flag = 0;
  sd.option    = NULL;
  mtn_set_string((uint8_t *)path, &sd);
  mtn_process(members, &sd, (MTNPROCFUNC)mtn_readlink_process);
  del_members(members);
  snprintf(buff, size, "%s", sd.option);
  xfree(sd.option);
  return(0);
}

void mtn_chmod_process(kmember *member, kdata *sd, kdata *rd, kaddr *addr)
{
	if(rd->head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, rd, sizeof(errno));
	  lprintf(0,"[error] %s: %s %s\n", __func__, member->host, strerror(errno));
  }
}

int mtn_chmod(const char *path, mode_t mode)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kdata   sd;
  kmember *m;
  if(is_mtnstatus(path, NULL)){
    return(-EACCES);
  }
  m = get_members();
	memset(&sd, 0, sizeof(sd));
  sd.head.type = MTNCMD_CHMOD;
  sd.head.size = 0;
  sd.head.flag = 0;
  mtn_set_string((uint8_t *)path, &sd);
  mtn_set_int(&mode, &sd, sizeof(mode));
  mtn_process(m, &sd, (MTNPROCFUNC)mtn_chmod_process);
  del_members(m);
  return(0);
}

void mtn_chown_process(kmember *member, kdata *sd, kdata *rd, kaddr *addr)
{
	if(rd->head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, rd, sizeof(errno));
	  lprintf(0,"[error] %s: %s %s\n", __func__, member->host, strerror(errno));
  }
}

int mtn_chown(const char *path, uid_t uid, gid_t gid)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kdata   sd;
  kmember *m;
  if(is_mtnstatus(path, NULL)){
    return(-EACCES);
  }
  m = get_members();
	memset(&sd, 0, sizeof(sd));
  sd.head.type = MTNCMD_CHOWN;
  sd.head.size = 0;
  sd.head.flag = 0;
  mtn_set_string((uint8_t *)path, &sd);
  mtn_set_int(&uid, &sd, sizeof(uid));
  mtn_set_int(&gid, &sd, sizeof(gid));
  mtn_process(m, &sd, (MTNPROCFUNC)mtn_chown_process);
  del_members(m);
  return(0);
}

void mtn_utime_process(kmember *member, kdata *sd, kdata *rd, kaddr *addr)
{
	if(rd->head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, rd, sizeof(errno));
	  lprintf(0,"[error] %s: %s %s\n", __func__, member->host, strerror(errno));
  }
}

int mtn_utime(const char *path, time_t act, time_t mod)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kdata   sd;
  kmember *m;
  if(is_mtnstatus(path, NULL)){
    return(-EACCES);
  }
  m = get_members();
	memset(&sd, 0, sizeof(sd));
  sd.head.type = MTNCMD_UTIME;
  sd.head.size = 0;
  sd.head.flag = 0;
  mtn_set_string((uint8_t *)path, &sd);
  mtn_set_int(&act, &sd, sizeof(act));
  mtn_set_int(&mod, &sd, sizeof(mod));
  mtn_process(m, &sd, (MTNPROCFUNC)mtn_utime_process);
  del_members(m);
  return(0);
}

//-------------------------------------------------------------------
// TCP
//-------------------------------------------------------------------
int mtn_connect(const char *path, int create_flag)
{
  int s;
  uint64_t dfree;
  char ipstr[64];
  kstat *st;
	lprintf(2, "[debug] %s:\n", __func__);
  st = mtn_find(path, create_flag);
  if(st == NULL){
    lprintf(0, "[error] %s: node not found\n", __func__);
    errno = EACCES;
    return(-1);
  }
  s = create_socket(0, SOCK_STREAM);
  if(s == -1){
    lprintf(0, "[error] %s:\n", __func__);
    delstats(st);
    errno = EACCES;
    return(-1);
  }
  v4addr(&(st->member->addr), ipstr, sizeof(ipstr));
  if(connect(s, &(st->member->addr.addr.addr), st->member->addr.len) == -1){
    lprintf(0, "[error] %s: %s %s:%d\n", __func__, strerror(errno), ipstr, v4port(&(st->member->addr)));
    close(s);
    delstats(st);
    errno = EACCES;
    return(-1);
  }
  dfree = st->member->dfree * st->member->bsize / 1024 / 1024;
  lprintf(2, "[debug] %s: %s %s (%lluM free) PATH=%s\n", __func__, st->member->host, ipstr, dfree, path);
  delstats(st);
  return(s);
}

int mtn_open(const char *path, int flags, mode_t mode)
{
	lprintf(2, "[debug] %s:\n", __func__);
  kdata sd;
  kdata rd;
  int s = mtn_connect(path, ((flags & O_CREAT) != 0));
  if(s == -1){
    return(-1);
  }
  sd.head.ver  = PROTOCOL_VERSION;
  sd.head.size = 0;
  sd.head.flag = 0;
  sd.head.type = MTNCMD_OPEN;
  mtn_set_string((uint8_t *)path, &sd);
  mtn_set_int(&flags, &sd, sizeof(flags));
  mtn_set_int(&mode,  &sd, sizeof(mode));
  if(send_data_stream(s, &sd) == -1){
    close(s);
    return(-1);
  }
  if(recv_data_stream(s, &rd) == -1){
    close(s);
    return(-1);
  }
  if(rd.head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, &rd, sizeof(errno));
    close(s);
    return(-1);
  }
  kopt.sendsize[s] = 0;
  kopt.sendbuff[s] = xrealloc(kopt.sendbuff[s], MTN_TCP_BUFFSIZE);
  return(s);
}

int mtn_read(int s, char *buf, size_t size, off_t offset)
{
  int r = 0;
  int    rs;
  kdata  sd;
  kdata  rd;
  while(is_loop && size){
    sd.head.ver  = PROTOCOL_VERSION;
    sd.head.size = 0;
    sd.head.flag = 0;
    sd.head.type = MTNCMD_READ;
    if(mtn_set_int(&size,   &sd, sizeof(size)) == -1){
      r = -EIO;
      break;
    }
    if(mtn_set_int(&offset, &sd, sizeof(offset)) == -1){
      r = -EIO;
      break;
    }
    if(send_recv_stream(s, &sd, &rd) == -1){
      r = -errno;
      break;
    }
    if(rd.head.size == 0){
      break;
    }
    rs = (rd.head.size > size) ? size : rd.head.size;
    memcpy(buf, &(rd.data), rs);
    size   -= rs;
    offset += rs;
    buf    += rs;
    r      += rs;
  }
  return(r);
}

int mtn_flush(int s)
{
  kdata sd;
  kdata rd;
  sd.head.ver  = PROTOCOL_VERSION;
  sd.head.type = MTNCMD_RESULT;
  sd.head.flag = 0;
  if(kopt.sendsize[s] == 0){
    return(0);
  }
  if(send_stream(s, kopt.sendbuff[s], kopt.sendsize[s]) == -1){
    lprintf(0, "[error] %s: send_stream %s\n", __func__, strerror(errno));
    return(-1);
  }
  kopt.sendsize[s] = 0;
  if(send_recv_stream(s, &sd, &rd) == -1){
    lprintf(0, "[error] %s: send_recv_stream %s\n", __func__, strerror(errno));
    return(-1);
  }else if(rd.head.type == MTNCMD_ERROR){
    mtn_get_int(&errno, &rd, sizeof(errno));
    lprintf(0, "[error] %s: %s\n", __func__, strerror(errno));
    return(-1);
  }
  return(0);
}

int mtn_write(int s, char *buf, size_t size, off_t offset)
{
  int r = 0;
  int    sz;
  kdata  sd;
  kdata  rd;
  sz = size;
  sd.head.ver  = PROTOCOL_VERSION;
  sd.head.type = MTNCMD_WRITE;
  sd.head.flag = 1;
  while(size){
    sd.head.size = 0;
    mtn_set_int(&offset, &sd, sizeof(offset));
    r = mtn_set_data(buf, &sd, size);
    size   -= r;
    buf    += r;
    offset += r;
    if(kopt.sendsize[s] + sd.head.size + sizeof(sd.head) > MTN_TCP_BUFFSIZE){
      if(mtn_flush(s) == -1){
        r = -errno;
        char peer[INET_ADDRSTRLEN];
        socklen_t addr_len;
        struct sockaddr_in addr;
        addr_len = sizeof(addr);
        getpeername(s, (struct sockaddr *)(&addr), &addr_len);
        inet_ntop(AF_INET, &(addr.sin_addr), peer, INET_ADDRSTRLEN);
        lprintf(0, "[error] %s: %s %s\n", __func__, strerror(-r), peer);
        return(r);
      }
    }
    memcpy(kopt.sendbuff[s] + kopt.sendsize[s], &(sd.head), sizeof(sd.head));
    kopt.sendsize[s] += sizeof(sd.head);
    memcpy(kopt.sendbuff[s] + kopt.sendsize[s], &(sd.data), sd.head.size);
    kopt.sendsize[s] += sd.head.size;
  }
  return(sz);
}

int mtn_close(int s)
{
  int r = 0;
  kdata  sd;
  kdata  rd;
	lprintf(2, "[debug] %s:\n", __func__);
  if(s == 0){
    return(0);
  }
  sd.head.ver  = PROTOCOL_VERSION;
  sd.head.type = MTNCMD_CLOSE;
  sd.head.size = 0;
  sd.head.flag = 0;
  if(mtn_flush(s) == -1){
    r = -errno;
  }
  if(send_recv_stream(s, &sd, &rd) == -1){
    r = -errno;
  }else if(rd.head.type == MTNCMD_ERROR){
    mtn_set_int(&errno, &rd, sizeof(errno));
    r = -errno;
  }
  if(close(s) == -1){
    r = -errno;
  }
  return(r);
}

int mtn_callcmd(ktask *kt)
{
  kt->send.head.ver  = PROTOCOL_VERSION;
  kt->send.head.type = kt->type;
  if(kt->con){
    kt->res = send_recv_stream(kt->con, &(kt->send), &(kt->recv));
  }else{
    kt->con = mtn_connect(kt->path, kt->create);
    if(kt->con == -1){
      kt->res = -1;
      lprintf(0, "[error] %s: cat't connect %s\n", __func__, kt->path);
    }else{
      kt->res = send_recv_stream(kt->con, &(kt->send), &(kt->recv));
      close(kt->con);
    }
    kt->con = 0;
  }
  if((kt->res == 0) && (kt->recv.head.type == MTNCMD_ERROR)){
    kt->res = -1;
    mtn_get_int(&errno, &(kt->recv), sizeof(errno));
    lprintf(0, "[error] %s: %s\n", __func__, strerror(errno));
  }
  return(kt->res);
}

//-------------------------------------------------------------------
// mtntool
//-------------------------------------------------------------------
int mtn_get(int f, char *path)
{
  int s;
  kdata sd;
  kdata rd;
  lprintf(9,"[debug] %s: IN\n", __func__);

  s = mtn_open(path, O_RDONLY, 0644);
  if(s == -1){
    return(-1);
  }
  sd.head.ver  = PROTOCOL_VERSION;
  sd.head.size = 0;
  sd.head.flag = 0;
  sd.head.type = MTNCMD_GET;
  mtn_set_string(path, &sd);
  send_data_stream(s,  &sd);
  sd.head.size = 0;
  sd.head.type = MTNCMD_SUCCESS;
  while(is_loop){
    if(recv_data_stream(s, &rd) == -1){
      break;
    }
    if(rd.head.size == 0){
      break;
    }
    write(f, rd.data.data, rd.head.size);
  }
  send_data_stream(s, &sd);
  lprintf(9,"[debug] %s: OUT\n", __func__);
  return(0);
}

int mtn_set(int f, char *path)
{
  int s;
  int r;
  kdata sd;
  kdata rd;
  kstat st;
  struct timeval tv;
  
  if(fstat(f, &(st.stat)) == -1){
    gettimeofday(&tv, NULL);
    st.stat.st_uid   = getuid();
    st.stat.st_gid   = getgid();
    st.stat.st_mode  = 0640;
    st.stat.st_atime = tv.tv_sec;
    st.stat.st_mtime = tv.tv_sec;
  }
  s = mtn_open(path, O_WRONLY | O_CREAT , st.stat.st_mode);
  if(s == -1){
    return(-1);
  }
  sd.head.fin  = 0;
  sd.head.ver  = PROTOCOL_VERSION;
  sd.head.type = MTNCMD_SET;
  while(r = read(f, sd.data.data, sizeof(sd.data.data))){
    if(r == -1){
      return(-1);
    }
    sd.head.size = r;
    r = send_recv_stream(s, &sd, &rd);
    if(r == -1){
      return(-1);
    }
    if(rd.head.type == MTNCMD_ERROR){
      mtn_get_int(&errno, &rd, sizeof(errno));
      return(-1);
    }
  }
  return(0);
}

//-------------------------------------------------------------------
//
// mtnstatus
//
//-------------------------------------------------------------------
void kcount(int ddelta, int sdelta, int mdelta)
{
  pthread_mutex_lock(&(count_mutex));
  dcount += ddelta;
  scount += sdelta;
  mcount += mdelta;
  pthread_mutex_unlock(&(count_mutex));
}

char *get_mtnstatus_members()
{
  char *p = NULL;
  pthread_mutex_lock(&(status_mutex));
  if(kopt.mtnstatus_members.buff){
    p = xmalloc(kopt.mtnstatus_members.size);
    strcpy(p, kopt.mtnstatus_members.buff);
  }
  pthread_mutex_unlock(&(status_mutex));
  return(p); 
}

char *get_mtnstatus_debuginfo()
{
  char *p = NULL;
  pthread_mutex_lock(&(status_mutex));
  if(kopt.mtnstatus_debuginfo.buff){
    p = xmalloc(kopt.mtnstatus_debuginfo.size);
    strcpy(p, kopt.mtnstatus_debuginfo.buff);
  }
  pthread_mutex_unlock(&(status_mutex));
  return(p); 
}

char *get_mtnstatus_debuglevel()
{
  char *p = NULL;
  pthread_mutex_lock(&(debug_mutex));
  if(kopt.mtnstatus_debuglevel.buff){
    p = xmalloc(kopt.mtnstatus_debuglevel.size);
    strcpy(p, kopt.mtnstatus_debuglevel.buff);
  }
  pthread_mutex_unlock(&(debug_mutex));
  return(p); 
}

size_t mtnstatus_members()
{
  char   **buff;
  size_t  *size;
  size_t result;
  uint64_t dsize;
  uint64_t dfree;
  uint64_t pfree;
  uint64_t vsz;
  uint64_t res;
  char ipstr[64];
  kmember *mb;
  kmember *members;
  pthread_mutex_lock(&(status_mutex));
  buff = &(kopt.mtnstatus_members.buff);
  size = &(kopt.mtnstatus_members.size);
  if(*buff){
    **buff = 0;
  }
  members = mtn_info();
  //exsprintf(buff, size, "%-10s %-15s %s %s %s %s %s\n", "Host", "IP", "Free[%]", "Free[MB]", "Total[MB]", "VSZ", "RSS");
  for(mb=members;mb;mb=mb->next){
    dsize = (mb->dsize * mb->fsize - mb->limit) / 1024 / 1024;
    dfree = (mb->dfree * mb->bsize - mb->limit) / 1024 / 1024;
    pfree = dfree * 100 / dsize;
    vsz   = mb->vsz / 1024;
    res   = mb->res / 1024;
    v4addr(&(mb->addr), ipstr, sizeof(ipstr));
    exsprintf(buff, size, "%s %s %d %2d%% %llu %llu %llu %llu %d\n", 
      mb->host,
      ipstr, 
      mb->membercnt,
      pfree, 
      dfree, 
      dsize, 
      vsz, 
      res, 
      mb->malloccnt); 
  }
  del_members(members);
  result = (*buff == NULL) ? 0 : strlen(*buff);
  pthread_mutex_unlock(&(status_mutex));
  return(result);
}

size_t mtnstatus_debuginfo()
{
  kdir    *kd;
  kstat   *ks;
  kmember *km;
  char   **buff;
  size_t  *size;
  size_t result;
  meminfo minfo;

  pthread_mutex_lock(&(count_mutex));
  pthread_mutex_lock(&(status_mutex));
  buff = &(kopt.mtnstatus_debuginfo.buff);
  size = &(kopt.mtnstatus_debuginfo.size);
  if(*buff){
    **buff = 0;
  }
  get_meminfo(&minfo);
  exsprintf(buff, size, "[DEBUG INFO]\n");
  exsprintf(buff, size, "VSZ   : %llu KB\n", minfo.vsz / 1024);
  exsprintf(buff, size, "RSS   : %llu KB\n", minfo.res / 1024);
  exsprintf(buff, size, "MALLOC: %d\n", malloccnt());
  exsprintf(buff, size, "DIR   : %d\n", dcount);
  exsprintf(buff, size, "STAT  : %d\n", scount);
  exsprintf(buff, size, "MEMBER: %d\n", mcount);
  exsprintf(buff, size, "RCVBUF: %d\n", kopt.rcvbuf);
  for(kd=kopt.dircache;kd;kd=kd->next){
    exsprintf(buff, size, "THIS=%p PREV=%p NEXT=%p FLAG=%d PATH=%s\n", kd, kd->prev, kd->next, kd->flag, kd->path);
    for(ks=kd->kst;ks;ks=ks->next){
      exsprintf(buff, size, "  THIS=%p PREV=%p NEXT=%p \t MEMBER=%p HOST=%s NAME=%s\n", ks, ks->prev, ks->next, ks->member, ks->member->host, ks->name);
    }
  }
  result = strlen(*buff);
  pthread_mutex_unlock(&(status_mutex));
  pthread_mutex_unlock(&(count_mutex));
  return(result);
}

size_t mtnstatus_debuglevel()
{
  char  **buff;
  size_t *size;
  size_t   len;

  pthread_mutex_lock(&(debug_mutex));
  buff = &(kopt.mtnstatus_debuglevel.buff);
  size = &(kopt.mtnstatus_debuglevel.size);
  if(*buff){
    **buff = 0;
  }
  len = exsprintf(buff, size, "%d\n", kopt.debuglevel);
  pthread_mutex_unlock(&(debug_mutex));
  return(len);
}

