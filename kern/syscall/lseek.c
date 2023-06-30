#include <types.h>
#include <lib.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/stat.h>
#include <copyinout.h>
#include <current.h>
#include <thread.h>
#include <kern/include/syscall.h>


off_t sys_lseek(int fd, off_t pos, int whence){
    if(fd<0 || fd>= OPEN_MAX || curproc->fileTable == NULL){
        return EBADF; //invalid file descriptor
    }
    struct openfile *file = curproc->fileTable[fd];
    switch(whence){
        case SEEK_SET:
            file->offset = pos;
            break;
        case SEEK_CUR:
            file->offset +=pos;
            break;
        case SEEK_END:
            file->offset = file->vn->vn_len+pos;
            break;
        default:
            return EINVAL; //invalid whence value
    }

    return file->offset;
}