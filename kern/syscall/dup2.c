#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/limits.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <vfs.h>

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

    int err = fdtable_dup(curproc->fileTable,oldf,newfd);
    if(err){
        return err;
    }
    return newfd;
    
}

int fdtable_dup(struct *openfile[] fdt, int oldfd, int newfd){
    struct openfile * new_file;
    struct openfile * old_file;
    int err;

    KASSERT(fdt != NULL);
    KASSERT(oldfd >= 0 && oldfd <__OPEN_MAX);
    KASSERT(newfd >= 0 && newfd <__OPEN_MAX);

    old_file = fdt[oldfd];

    if(old_file == NULL){
        return EBADF;
    }

    new_file = kmalloc(sizeof(struct openfile));
    if(new_file == NULL){
        return ENOMEN;
    }

    err = file_dup(old_file,new_file);
    if(err){
        kfree(new_file);
        return err;
    }

    fdt[newfd] = new_file;
    return 0;
    
}

int file_dup(struct openfile * old_file,struct openfile * new_file){
    KASSERT(old_file != NULL);
    KASSERT(new_file != NULL);

    new_file->vn = old_file->vn;
    old_file->vn->offset++;
    new_file->offset = old_file->offset;
    new_file->countRef = old_file->countRef;

    return 0;
    
}