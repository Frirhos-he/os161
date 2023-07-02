#include <types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#define USE_KERNEL_BUFFER 0

struct openfile systemFileTable[SYSTEM_OPEN_MAX];



#if USE_KERNEL_BUFFER

static int
file_write(int fd, userptr_t buf_ptr, size_t size) {
  struct iovec iov;
  struct uio ku;
  int result, nwrite;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd<0||fd>OPEN_MAX) return -1;
  of = curproc->fileTable[fd];
  if (of==NULL) return -1;
  vn = of->vn;
  if (vn==NULL) return -1;

  off_t remaining = vn->vn_len - of->offset;//remaining length of the file
  size_t bytes_to_write = (size < (size_t)remaining) ? size : (size_t)remaining;
  //bytes_to_read is the lower between remaining and size
  

  kbuf = kmalloc(bytes_to_write);
  copyin(buf_ptr,kbuf,bytes_to_write);
  uio_kinit(&iov, &ku, kbuf, bytes_to_write, of->offset, UIO_WRITE);
  result = VOP_WRITE(vn, &ku);
  if (result) {
    return result;
  }
  kfree(kbuf);
  of->offset = ku.uio_offset;
  nwrite = bytes_to_write - ku.uio_resid;
  return (nwrite);
}

#else

static int
file_write(int fd, userptr_t buf_ptr, size_t size) {
  struct iovec iov;
  struct uio u;
  int result, nwrite;
  struct vnode *vn;
  struct openfile *of;

  if (fd<0||fd>OPEN_MAX) return -1;
  of = curproc->fileTable[fd];
  if (of==NULL) return -1;
  vn = of->vn;
  if (vn==NULL) return -1;

  off_t remaining = vn->vn_len - of->offset;//remaining length of the file
  size_t bytes_to_write = (size < (size_t)remaining) ? size : (size_t)remaining;
  //bytes_to_read is the lower between remaining and size
  

  iov.iov_ubase = buf_ptr;
  iov.iov_len = bytes_to_write;

  u.uio_iov = &iov;
  u.uio_iovcnt = 1;
  u.uio_resid = bytes_to_write;          // amount to read from the file
  u.uio_offset = of->offset;
  u.uio_segflg =UIO_USERISPACE;
  u.uio_rw = UIO_WRITE;
  u.uio_space = curproc->p_addrspace;

  result = VOP_WRITE(vn, &u);
  if (result) {
    return result;
  }
  of->offset = u.uio_offset;
  nwrite = bytes_to_write - u.uio_resid;
  return (nwrite);
}

#endif

int
sys_write(int fd, userptr_t buf_ptr, size_t size)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd!=STDOUT_FILENO && fd!=STDERR_FILENO) {
    kprintf("sys_write supported only to stdout\n");
    return -1;
  }

  for (i=0; i<(int)size; i++) {
    putch(p[i]);
  }

  return (int)size;
}

