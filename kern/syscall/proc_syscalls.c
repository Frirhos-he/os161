#include <types.h>
#include <spl.h>
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
#include <vfs.h>
#include <vnode.h>
#include <kern/fcntl.h>
#include <kern/seek.h>

int copyin_args(const char *progname, char **args, int num_args, char **kargs);
int copyout_args(char ** kargs, int num_args, vaddr_t * stackptr);
void enter_new_process_(int argc,userptr_t stackptr,userptr_t entrypoint, vaddr_t stacktop, vaddr_t entryaddr);


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
    thread_exit();

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

static void
call_enter_forked_process(void *tfv, unsigned long dummy) {
  struct trapframe *tf = (struct trapframe *)tfv;
  (void)dummy;
  enter_forked_process(tf); 
 
  panic("enter_forked_process returned (should not happen)\n");
}

int sys_fork(struct trapframe *ctf, pid_t *retval) {
    struct trapframe *tf_child;
    struct proc *newp;
    int result;

    KASSERT(curproc != NULL);

    newp = proc_create_runprogram(curproc->p_name);
    if (newp == NULL) {
        return ENOMEM;
    }

    /* done here as we need to duplicate the address space 
       of the current process */
    as_copy(curproc->p_addrspace, &(newp->p_addrspace));
    if(newp->p_addrspace == NULL){
        proc_destroy(newp); 
        return ENOMEM; 
    }

    /* we need a copy of the parent's trapframe */
    tf_child = kmalloc(sizeof(struct trapframe));
    if(tf_child == NULL){
        proc_destroy(newp);
        return ENOMEM; 
    }
    memcpy(tf_child, ctf, sizeof(struct trapframe));

    /* TO BE DONE: linking parent/child, so that child terminated 
     on parent exit */

    result = thread_fork(
        curthread->t_name, newp,
		call_enter_forked_process, 
		(void *)tf_child, (unsigned long)0/*unused*/);

  if (result){
    proc_destroy(newp);
    kfree(tf_child);
    return ENOMEM;
  }

  *retval = newp->p_pid;

  return 0;
}

int sys_execv( char * progname, char * args[]){

    struct addrspace *as;
    struct vnode *v;
    vaddr_t entrypoint,stackptr;
    int result;

    //progname=progname+1;


    int num_args = 0;
    while(args[num_args] != NULL){
        num_args++;
    }

    char **kargs = kmalloc((num_args+1)*sizeof(char *));
    if(kargs == NULL){
        return ENOMEM;
    }

    result = copyin_args(progname,args,num_args,kargs);
    

    if(result){
        kfree(args); //why must be freed inside this function?
        return result;
    }

    //open the executable file
    result = vfs_open(kargs[0],O_RDONLY,0,&v);

    if(result){
        kfree(args);
        return result;
    }
    //create a new address space
    as = proc_getas();
    as_destroy(as);

    as = as_create();
    if(as == NULL){
        vfs_close(v);
        kfree(args);
        return ENOMEM;
    }

    //switch to the new address space
    proc_setas(as);
    as_activate(); 

    //load the executable

    result = load_elf(v,&entrypoint);
    if(result){
        vfs_close(v);
        kfree(args);
        return result;
    }

    //done with the file
    vfs_close(v);


    //define the user stack in the address space
    result = as_define_stack(as,&stackptr);
    if(result){
        kfree(args);
        return result;
    }

    //copy arguments to the user stack
    result = copyout_args(kargs,num_args,&stackptr);
    if(result){
        kfree(args);
        return result;
    }
/*
    //free the kernel-space argument array
  //  kfree(args);

    //warp to user mode
    enter_new_process_(num_args,(userptr_t)stackptr,NULL,stackptr,entrypoint);*/
    enter_new_process(num_args,(userptr_t)stackptr,NULL,stackptr,entrypoint);

    //enter_new_process does not return
    panic("enter new process returned\n");
    return EINVAL;
}

int copyin_args(const char *progname, char **args, int num_args, char **kargs){
    
    int result,i,j;

    kargs[0] = kmalloc((strlen(progname)+1)*sizeof(char));
    if(kargs[0] == NULL) return ENOMEM;

    //copy programname
    result = copyinstr((const userptr_t)progname,kargs[0],(strlen(progname)+1)*sizeof(char),NULL);
    if(result){
        return result;
    }

    //copy arguments
    for(i = 0; i < num_args; i++){
        kargs[i+1] = kmalloc((strlen(args[i])+1)*sizeof(char));

        if(kargs[i+1] == NULL){
            for (j = 0 ; j<i+1;j++){
                kfree(kargs[j]);
            }
            return ENOMEM;
        }

        result = copyinstr((const userptr_t) args[i],kargs[i+1],(strlen(args[i])+1)*sizeof(char),NULL);
        if(result){
            for (j = 0 ; j<i+1;j++){
                kfree(kargs[j]);
            }
            return result;
        }
    }
    kargs[num_args+1] = NULL;

    return 0;

}
int copyout_args(char ** kargs, int num_args, vaddr_t * stackptr){
    int result,i;
    vaddr_t argv[num_args+1];

    //copy arguments to user stack in reverse order
    for (i = num_args -1; i >= 0; i--){

        size_t arglen = strlen(kargs[i])+1;
        //align the stack pointer
        *stackptr -= ROUNDUP(arglen,sizeof(void*));
        result = copyoutstr(kargs[i],(userptr_t)(*stackptr),arglen,NULL);
        if(result){
            return result;
        }
        argv[i] = *stackptr;
    }

    //copy argument pointers to user stack
    *stackptr -= ROUNDUP ((num_args+1)*sizeof(void*),sizeof(void*));
    result = copyout(argv,(userptr_t)(*stackptr),(num_args+1)*sizeof(void*));
    if(result){
        return result;
    }

    return 0;
}

void enter_new_process_(int argc,userptr_t stackptr,userptr_t entrypoint, vaddr_t stacktop, vaddr_t entryaddr){

    (void)argc;
    (void)stacktop;
    (void)entryaddr;

    struct trapframe tf;
    struct addrspace *as;
    int spl;
    int err;

    (void)entrypoint;//not used in this implementation

    as_deactivate();
    as = proc_setas(NULL);
    as_destroy(as);

    curproc_cleanup((void*)NULL);

    spl = splhigh(); //disable interrupts temporarily

    //copy the contents of the trapframe from the stack pointer to the local trapframe
    tf = *(struct trapframe*)stackptr;
    kfree((void*)stackptr);

    //update the registers of the child's trapframe
    tf.tf_a3 = 0;
    tf.tf_v0 = 0;
    tf.tf_epc +=4;

    //activate the new process' addres space
    err= as_copy(curproc->p_addrspace,&as);
    if(err){
        panic("enter_new_process: error on as copy\n");
    }
    splx(spl);
    proc_setas(as);
    as_activate();
    call_enter_forked_process((void*)&tf,(unsigned long)0 /*unused*/);

    panic("enter_new_process: enter_forked_process returned\n");

}