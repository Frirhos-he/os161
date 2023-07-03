#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <syscall.h>
#include <current.h>
#include <lib.h>
#include <copyinout.h>
#include <vnode.h>
#include <vfs.h>
#include <limits.h>
#include <uio.h>
#include <proc.h>
#include <kern/stat.h>
#include <thread.h>
#include <kern/include/syscall.h>

/* max num of system wide open files */
#define SYSTEM_OPEN_MAX (10*OPEN_MAX)

#define USE_KERNEL_BUFFER 0

/* system open file table */
struct openfile {
  struct vnode *vn;
  off_t offset;	
  unsigned int countRef;
};

struct openfile systemFileTable[SYSTEM_OPEN_MAX];

void openfileIncrRefCount(struct openfile *of) {
  if (of!=NULL)
    of->countRef++;
}

//open and close syscalls

int
sys_open(userptr_t path, int openflags, mode_t mode, int *errp)
{
    int fd, i;
    struct vnode *v;
    struct openfile *of=NULL;; 	
    int result;

    //open file
    result = vfs_open((char *)path, openflags, mode, &v);
    if (result) {
      *errp = ENOENT;
      return -1;
      }

    //saving stats about file in file_stat
    struct stat = file_stat;
    result = VOP_STAT(vn, &file_stat);
    if(result){
      *errp = ENOENT;
      return -1;
    }

    //saving the length of the file in the related vnode
    off_t file_size = file_stat.st_size;
    v->vn_len = file_size; 

    /* search system open file table */
    for (i=0; i<SYSTEM_OPEN_MAX; i++) {
      if (systemFileTable[i].vn==NULL) {
        of = &systemFileTable[i];
        of->vn = v;
        of->offset = 0; // TODO: handle offset with append
        if(openflags && O_APPEND != 0){
            //if I have the APPEND flag, update the offset
            of->offset = v->vn_len;
        }
        of->countRef = 1;
        break;
      }
    }
    if (of==NULL) { 
        // no free slot in system open file table
        *errp = ENFILE;
  }
  else {
    for (fd=STDERR_FILENO+1; fd<OPEN_MAX; fd++) {
        if (curproc->fileTable[fd] == NULL) {
            curproc->fileTable[fd] = of;
	        return fd;
      }
    }
    // no free slot in process open file table
    *errp = EMFILE;
  }
  
  vfs_close(v);
  return -1;
}

int
sys_close(int fd){

    struct openfile *of=NULL; 
    struct vnode *vn;

    if (fd<0||fd>OPEN_MAX)
        return -1;

    of = curproc->fileTable[fd];

    if (of==NULL)
        return -1;

    //remove from curproc fileTable
    curproc->fileTable[fd] = NULL; 
  

    if (--of->countRef > 0) 
        return 0; // just decrement ref cnt
    vn = of->vn;
    of->vn = NULL;
    if (vn==NULL) return -1;

    vfs_close(vn);	
    return 0;
}

//read and write syscalls

#if USE_KERNEL_BUFFER
static int
file_read(int fd, userptr_t buf_ptr, size_t size) {
    struct iovec iov;
    struct uio ku;
    int result, nread;
    struct vnode *vn;
    struct openfile *of;
    void *kbuf;

    if (fd<0||fd>OPEN_MAX)
        return -1;

    of = curproc->fileTable[fd];
    if (of==NULL) 
        return -1;

    vn = of->vn;
    if (vn==NULL)
        return -1;

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

    if (fd<0||fd>OPEN_MAX)
        return -1;
  
    of = curproc->fileTable[fd];
    if (of==NULL)
        return -1;

    vn = of->vn;
    if (vn==NULL)   
        return -1;

    off_t remaining = vn->vn_len - of->offset;//remaining length of the file
    size_t bytes_to_read = (size < (size_t)remaining) ? size : (size_t)remaining;
    //bytes_to_read is the lower between remaining and size
  
    //initialize iovec and uio
    siov.iov_ubase = buf_ptr;
    iov.iov_len = bytes_to_read;

    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_resid = bytes_to_read;   // amount to read from the file
    u.uio_offset = of->offset;
    u.uio_segflg =UIO_USERISPACE;
    u.uio_rw = UIO_READ;
    u.uio_space = curproc->p_addrspace;

    //perform read
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

  //reading a file
  if (fd!=STDIN_FILENO) {
    return file_read(fd, buf_ptr, size);
  }

  //stdin
  for (i=0; i<(int)size; i++) {
    p[i] = getch();
    if (p[i] < 0) 
      return i;
  }

  return (int)size;
}

#if USE_KERNEL_BUFFER

static int
file_write(int fd, userptr_t buf_ptr, size_t size) {
    struct iovec iov;
    struct uio ku;
    int result, nwrite;
    struct vnode *vn;
    struct openfile *of;
    void *kbuf;

    if (fd<0||fd>OPEN_MAX)
        return -1;
  
    of = curproc->fileTable[fd];
    if (of==NULL)
        return -1;
    
    vn = of->vn;
    if (vn==NULL)   
        return -1;

    off_t remaining = vn->vn_len - of->offset;//remaining length of the file
    size_t bytes_to_write = (size < (size_t)remaining) ? size : (size_t)remaining;
    //bytes_to_write is the lower between remaining and size
  

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

    if (fd<0||fd>OPEN_MAX)
        return -1;
  
    of = curproc->fileTable[fd];
    if (of==NULL)
        return -1;

    vn = of->vn;
    if (vn==NULL) 
        return -1;

    off_t remaining = vn->vn_len - of->offset;//remaining length of the file
    size_t bytes_to_write = (size < (size_t)remaining) ? size : (size_t)remaining;
    //bytes_to_write is the lower between remaining and size
  
    //initializing iovec and uio
    iov.iov_ubase = buf_ptr;
    iov.iov_len = bytes_to_write;

    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_resid = bytes_to_write;  // amount of bytes we want to write into the file
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

    //write into a file
    if (fd!=STDOUT_FILENO && fd!=STDERR_FILENO) {
        return file_write(fd, buf_ptr, size);
    }

    //write to stdout ot stderr
    for (i=0; i<(int)size; i++) {
        putch(p[i]);
        }

    return (int)size;
}


//lseek syscall

off_t sys_lseek(int fd, off_t pos, int whence){

    if(fd<0 || fd>= OPEN_MAX || curproc->fileTable[fd] == NULL){
        return EBADF; //invalid file descriptor
    }

    switch(whence){
        case SEEK_SET:
            curproc->fileTable[fd]->offset = pos;
            break;
        case SEEK_CUR:
            curproc->fileTable[fd]->offset +=pos;
            break;
        case SEEK_END:
            curproc->fileTable[fd]->offset = curproc->fileTable[fd]->vn->vn_len+pos;
            break;
        default:
            return EINVAL; //invalid whence value
    }

    return file->offset;
}


//dup2 syscall

int sys_dup2(int oldfd, int newfd){

    int result;

    //check validity of the two file descriptors
    if(oldfd < 0 || oldfd >= __OPEN_MAX || curproc->fileTable[oldfd] == NULL){
        return EBADF;
    }
    if(newfd == oldfd)
        return oldfd; //same fd (should be an error ?)
    if(newfd < 0 || newfd >= __OPEN_MAX || curproc->fileTable[newfd] != NULL){
        return EBADF;
    }

    //perform the dup2 
    int err = fdtable_dup(curproc->fileTable,oldf,newfd);
    if(err){
        return err;
    }
    return newfd;
    
}

int fdtable_dup(int oldfd, int newfd){
    struct openfile * new_file;
    struct openfile * old_file;
    int err;

    KASSERT(fdt != NULL);
    KASSERT(oldfd >= 0 && oldfd <__OPEN_MAX);
    KASSERT(newfd >= 0 && newfd <__OPEN_MAX);

    old_file = curproc->fileTable[oldfd];

    if(old_file == NULL){
        return EBADF;
    }

    //allocate the new file
    new_file = kmalloc(sizeof(struct openfile));
    if(new_file == NULL){
        return ENOMEN;
    }

    //copy the old file fields in the new file
    err = file_dup(old_file,new_file);
    if(err){
        kfree(new_file);
        return err;
    }

    curproc->fileTable[newfd] = new_file;
    return 0;
    
}

int file_dup(struct openfile * old_file,struct openfile * new_file){
    KASSERT(old_file != NULL);
    KASSERT(new_file != NULL);

    new_file->vn = old_file->vn;
    new_file->offset = old_file->offset;
    new_file->countRef = old_file->countRef;

    openfileIncrRefCount(old_file);
    openfileIncrRefCount(new_file);

    return 0;
    
}

int sys_chdir(userptr_t path){

    if(path == NULL){
        return EFAULT;
    }

    char * kbuf;
    int result;

    //allocate kernel buffer
    kbuf = kmalloc(PATH_MAX);
    if(kbuf == NULL){
        return ENOMEM;
    }

    //copy the pathname from user space to kernel buffer
    result = copyinstr(path,kbuf,PATH_MAX,NULL);
    if(result){
        kfree(kbuf);
        return result;
    }

    //open the directory
    struct vnode *newcwd;
    result = vfs_ope(kbuf,O_READONLY,,0,&v);
    if(result){
        kfree(kbuf);
        return result;
    }

    struct vnode * oldcwd = curthread->t_cwd;
    curthread->t_cwd = newcwd;
    vfs_close(oldcwd);

    kfree(kbuf);
    return 0;

}