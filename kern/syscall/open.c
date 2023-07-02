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



int
sys_open(userptr_t path, int openflags, mode_t mode, int *errp)
{
  int fd, i;
  struct vnode *v;
  struct openfile *of=NULL;; 	
  int result;

  result = vfs_open((char *)path, openflags, mode, &v);
  if (result) {
    *errp = ENOENT;
    return -1;
  }

  struct stat = file_stat;
  result = VOP_STAT(vn, &file_stat);
  if(result){
    *errp = ENOENT;
    return -1;
  }

  off_t file_size = file_stat.st_size;
  v->vn_len = file_size;

  /* search system open file table */
  for (i=0; i<SYSTEM_OPEN_MAX; i++) {
    if (systemFileTable[i].vn==NULL) {
      of = &systemFileTable[i];
      of->vn = v;
      of->offset = 0; // TODO: handle offset with append
      if(openflags && O_APPEND != 0){
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