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
#include <kern/syscall.h>
#include <spinlock.h>
#include <kern/fcntl.h>
#include <kern/seek.h>


void openfileIncrRefCount(struct openfile *of) {
  if (of!=NULL){
    spinlock_acquire(of->countref_lk);
    of->countRef++;
    spinlock_release(of->countref_lk);
  }
}

int fdtable_dup(int oldfd, int newfd);

//open and close syscalls

int
sys_open(userptr_t path, int openflags, mode_t mode, int *errp)
{
    int fd, i;
    struct vnode *v;
    struct openfile *of=NULL;
    int result;
    userptr_t path_temp= path;

    //open file
    result = vfs_open((char *)path_temp, openflags, mode, &v);
    if (result) {
      *errp = result; //manual reference
      return -1;
      }

    //saving stats about file in file_stat
    struct stat file_stat;
    result = VOP_STAT(v, &file_stat);
    if(result){
      *errp = result; // manual refernce
      vfs_close(v);
      return -1;
    }

    //saving the length of the file in the related vnode
    off_t file_size = file_stat.st_size;
    v->vn_len = file_size; 

    /* search system open file table */
    for (i=0; i<SYSTEM_OPEN_MAX; i++) {
      if (systemFileTable[i].vn==NULL) {
        of = &systemFileTable[i];
        of->countref_lk=kmalloc(sizeof(struct spinlock));
        spinlock_init(of->countref_lk);
        of->vn = v;
        of->offset = 0; 
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
        vfs_close(v);
        return -1;
    }
    else {
        for (fd=STDERR_FILENO+1; fd<OPEN_MAX; fd++) {
            if (curproc->fileTable[fd] == NULL) {
                curproc->fileTable[fd] = of;
                return fd;
            }
        }
        
        
    }
    // no free slot in process open file table
    *errp = EMFILE;
    of->vn = NULL;
    spinlock_cleanup(of->countref_lk);
    kfree(of->countref_lk);
    vfs_close(v);
    return -1;
}

int
sys_close(int fd){

    struct openfile *of=NULL; 
    struct vnode *vn;


    if (fd<0||fd>OPEN_MAX)
        return EBADF; // manual reference invalid file descriptor

    of = curproc->fileTable[fd];

    if (of==NULL)
        return EBADF; //Manual reference invalid file descriptor

    //remove from curproc fileTable
    curproc->fileTable[fd] = NULL; 
  

    if (--of->countRef > 0) 
        return 0; // just decrement ref cnt
    vn = of->vn;
    if (vn==NULL) return EBADF; //invalid vnode
    of->vn = NULL;
    spinlock_cleanup(of->countref_lk);
    kfree(of->countref_lk);
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
        return EBADF; //manual reference

    of = curproc->fileTable[fd];
    if (of==NULL) 
        return EBADF;

    vn = of->vn;
    if (vn==NULL)
        return EBADF;

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
        return EBADF;
  
    of = curproc->fileTable[fd];
    if (of==NULL)
        return EBADF;

    vn = of->vn;
    if (vn==NULL)   
        return EBADF;

    off_t remaining = vn->vn_len - of->offset;//remaining length of the file
    size_t bytes_to_read = (size < (size_t)remaining) ? size : (size_t)remaining;
    //bytes_to_read is the lower between remaining and size
  
    //initialize iovec and uio
    iov.iov_ubase = buf_ptr;
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

  //reading a file
  if (fd!=STDIN_FILENO) {
    return file_read(fd, buf_ptr, size);
  }

  //stdin
  char* temp_buf= kmalloc((size)*sizeof(char));
  if(temp_buf==NULL){
    return ENOMEM;
  }

  for (i=0; i<(int)size; i++) {
    temp_buf[i] = getch();
    if (temp_buf[i] < 0){
      int result= copyout(temp_buf,(userptr_t)buf_ptr,(size_t)i);
      return result;
    }
  }
  copyout(temp_buf,(userptr_t)buf_ptr,size);
  kfree(temp_buf);

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
    int result;
    if (fd<0||fd>OPEN_MAX)
        return EBADF;
  
    of = curproc->fileTable[fd];
    if (of==NULL)
        return EBADF;
    
    vn = of->vn;
    if (vn==NULL)   
        return EBADF;

    off_t remaining = vn->vn_len - of->offset;//remaining length of the file
    size_t bytes_to_write = (size < (size_t)remaining) ? size : (size_t)remaining;
    //bytes_to_write is the lower between remaining and size
  

    kbuf = kmalloc(bytes_to_write);
    if(kbuf == NULL) return ENOMEM;
    result = copyin(buf_ptr,kbuf,bytes_to_write);
    if(result != 0){
        kfree(kbuf);
        return result;
    }
    uio_kinit(&iov, &ku, kbuf, bytes_to_write, of->offset, UIO_WRITE);
    result = VOP_WRITE(vn, &ku);
    if (result) {
        kfree(kbuf);
        return result;
    }

    kfree(kbuf);
    of->offset = ku.uio_offset;
    nwrite = bytes_to_write - ku.uio_resid;
    if(ku.uio_resid != 0)return ENOSPC; //no space left on file system

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
        return EBADF;
  
    of = curproc->fileTable[fd];
    if (of==NULL)
        return EBADF;

    vn = of->vn;
    if (vn==NULL) 
        return EBADF;

    size_t bytes_to_write= size;
  
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
    return nwrite;
}

#endif

int
sys_write(int fd, userptr_t buf_ptr, size_t size)
{
    int i;
    int result;
    //write into a file
    if (fd!=STDOUT_FILENO && fd!=STDERR_FILENO) {
        return file_write(fd, buf_ptr, size);
    }

    
    //write to stdout ot stderr
    char* temp_buf= kmalloc((size+1)*sizeof(char));
    if(temp_buf==NULL){
    return ENOMEM;
    }
    result = copyin((userptr_t)buf_ptr, temp_buf, size+1);
    if(result != 0){
        kfree(temp_buf);
        return result; //invalid address space for buf
    }
    for (i=0; i<(int)size; i++) {
        putch(temp_buf[i]);
        }
    kfree(temp_buf);
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
    if(curproc->fileTable[fd]->offset <0 ){
        return EINVAL; //result of seek position negative
    }

    return curproc->fileTable[fd]->offset;
}

//dup syscall

int sys_dup(int oldfd){
    int i;
    int newfd=-1;

    //check validity of oldfd
    if(oldfd < 0 || oldfd >= __OPEN_MAX || curproc->fileTable[oldfd] == NULL){
        return EBADF;
    }

    //find lower unused fd inside fileTable
    for (i=0; i<OPEN_MAX; i++){
        if(curproc->fileTable[i]==NULL){
            newfd=i;
            int err= fdtable_dup(oldfd,newfd);
            if(err) return err;
        }
    }

    return newfd;
}


//dup2 syscall

int sys_dup2(int oldfd, int newfd){

    //check validity of the two file descriptors
    if(oldfd < 0 || oldfd >= __OPEN_MAX || curproc->fileTable[oldfd] == NULL){
        return EBADF;
    }
    if(newfd == oldfd)
        return oldfd; //same fd 
    if(newfd < 0 || newfd >= __OPEN_MAX ){
        return EBADF;
    }

    //if newfd already used, close corresponding file first
    if(curproc->fileTable[newfd]!=NULL){
        sys_close(newfd);
    }

    //perform the dup2 
    int err = fdtable_dup(oldfd,newfd);
    if(err){
        return err;
    }
    return newfd;
    
}

// auxiliary function for dup

int fdtable_dup(int oldfd, int newfd){
  
    struct openfile * old_file;
 
    KASSERT(oldfd >= 0 && oldfd <__OPEN_MAX);
    KASSERT(newfd >= 0 && newfd <__OPEN_MAX);

    old_file = curproc->fileTable[oldfd];

    if(old_file == NULL){
        return EBADF;
    }

    curproc->fileTable[newfd]=old_file;
    openfileIncrRefCount(old_file);

    return 0;
    
}

// chdir syscall

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
    result = vfs_open(kbuf,O_RDONLY,0,&newcwd);
    if(result){
        kfree(kbuf);
        return result;
    }

    struct vnode * oldcwd = curproc->p_cwd;
    curproc->p_cwd = newcwd;
    vfs_close(oldcwd);

    kfree(kbuf);
    return 0;

}

//getcwd syscall

int sys_getcwd(userptr_t buf_ptr,size_t size){
    struct uio u;
    struct iovec iov;
    
    if(buf_ptr == NULL){
        return EFAULT;
    }

    int result;

    iov.iov_ubase = buf_ptr;
    iov.iov_len = size;

    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_resid = size;  
    u.uio_offset = 0;
    u.uio_segflg =UIO_USERISPACE;
    u.uio_rw = UIO_READ;
    u.uio_space = curproc->p_addrspace;

    result = vfs_getcwd(&u);
    if(result){
        return result;
    }


    return size;
}