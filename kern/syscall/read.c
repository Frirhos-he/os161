#include <types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#define USE_KERNEL_BUFFER 0

#define SYSTEM_OPEN_MAX (10*OPEN_MAX)

#define USE_KERNEL_BUFFER 0

/* system open file table */
struct openfile {
  struct vnode *vn;
  off_t offset;	
  unsigned int countRef;
};

struct openfile systemFileTable[SYSTEM_OPEN_MAX];


#if USE_KERNEL_BUFFER

static int
file_read(int fd, userptr_t buf_ptr, size_t size) {
  struct iovec iov;
  struct uio ku;
  int result, nread;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd<0||fd>OPEN_MAX) return -1;
  of = curproc->fileTable[fd];
  if (of==NULL) return -1;
  vn = of->vn;
  if (vn==NULL) return -1;

  off_t remaining = vn->vn_len - of->offset;//remaining length of the file
  size_t bytes_to_read = (size < (size_t)remaining) ? size : (size_t)remaining;
  //bytes_to_read is the lower between remaining and size

  kbuf = kmalloc(bytes_to_read);
  uio_kinit(&iov, &ku, kbuf, bytes_to_read, of->offset, UIO_READ);
  result = VOP_READ(vn, &ku);
  if (result) {
    return result;
  }
  of->offset = ku.uio_offset;
  nread = bytes_to_read - ku.uio_resid;
  copyout(kbuf,buf_ptr,nread);
  kfree(kbuf);
  return (nread);
}

#else

static int
file_read(int fd, userptr_t buf_ptr, size_t size) {
  struct iovec iov;
  struct uio u;
  int result;
  struct vnode *vn;
  struct openfile *of;

  if (fd<0||fd>OPEN_MAX) return -1;
  of = curproc->fileTable[fd];
  if (of==NULL) return -1;
  vn = of->vn;
  if (vn==NULL) return -1;

  off_t remaining = vn->vn_len - of->offset;//remaining length of the file
  size_t bytes_to_read = (size < (size_t)remaining) ? size : (size_t)remaining;
  //bytes_to_read is the lower between remaining and size
  
  siov.iov_ubase = buf_ptr;
  iov.iov_len = bytes_to_read;

  u.uio_iov = &iov;
  u.uio_iovcnt = 1;
  u.uio_resid = bytes_to_read;   // amount to read from the file
  u.uio_offset = of->offset;
  u.uio_segflg =UIO_USERISPACE;
  u.uio_rw = UIO_READ;
  u.uio_space = curproc->p_addrspace;

  result = VOP_READ(vn, &u);
  if (result) {
    return result;
  }

  of->offset = u.uio_offset;
  return (bytes_to_read - u.uio_resid);
}
#endif

int
sys_read(int fd, userptr_t buf_ptr, size_t size)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd!=STDIN_FILENO) {
    return file_read(fd, buf_ptr, size);
  }

  for (i=0; i<(int)size; i++) {
    p[i] = getch();
    if (p[i] < 0) 
      return i;
  }

  return (int)size;
}
