#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <current.h>
#include <synch.h>

void
sys__exit(int status)
{
    struct proc *p = curproc;
    p->p_status = status & 0xff; /* just lower 8 bits returned */
    proc_remthread(curthread);
#if USE_SEMAPHORE_FOR_WAITPID
    V(p->p_sem);
#else
    lock_acquire(p->p_lock);
    cv_signal(p->p_cv);
    lock_release(p->p_lock);
#endif
    thread_exit(status);

    panic("thread_exit returned (should not happen)\n");
}
int
sys_waitpid(pid_t pid, userptr_t statusp, int options)
{
    struct proc *p = proc_search_pid(pid);
    int s;
    if (p==NULL) 
        return -1;
    s = proc_wait(p,options);
    if (statusp!=NULL) 
        *(int*)statusp = s;
    return pid;
}

pid_t
sys_getpid(void){
    
    KASSERT(curproc != NULL);
    return curproc->p_pid;
}
