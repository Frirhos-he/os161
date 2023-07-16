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
int copyout_args(char ** kargs, int num_args, vaddr_t * stackptr, vaddr_t * ptr_argv);
void rounddown(vaddr_t* stackptr, size_t sz );

// _exit syscall

void
sys__exit(int status)
{
    struct proc *p = curproc;
#if PC_LINK
    // make all the children processes exit
    if(p->num_children!=0) kill_children(p);

    struct proc* parent= p->parent;
    if (parent != NULL){        // if parent==NULL then this is kernel process (see 'proc_create')
        struct proc* child;
        for(int i=0; i<MAX_CHILDREN; i++){
            child=parent->children[i];
            if (child && strcmp(child->p_name,curproc->p_name)==0){
                parent->children[i]=NULL;
                break;
            }
        }
        parent->num_children--;
    }
#endif
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

#if PC_LINK
// variation of _exit used to kill children processes once the parent exits
void
exit_process(struct proc* p)
{
    // make all the children processes exit
    if(p->num_children==0) return;

    kill_children(p);

    struct thread* t;

    for(unsigned int i=0; i<MAX_THREADS; i++){
        t= p->p_threads[i];
        if( t && t->t_name!= curthread->t_name){
            proc_remthread(t);
            thread_destroy(p->p_threads[i]);
        }
    }
    
#if USE_SEMAPHORE_FOR_WAITPID
    V(p->p_sem);
#else
    lock_acquire(p->p_lock);
    cv_signal(p->p_cv);
    lock_release(p->p_lock);
#endif
    
}
#endif

// waitpid syscall

pid_t
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

// getpid syscall

pid_t
sys_getpid(void){
    
    KASSERT(curproc != NULL);
    return curproc->p_pid;
}

// auxiliary function called by fork

static void
call_enter_forked_process(void *tfv, unsigned long dummy) {
  struct trapframe *tf = (struct trapframe *)tfv;
  (void)dummy;
  enter_forked_process(tf); 
 
  panic("enter_forked_process returned (should not happen)\n");
}

//fork syscall

int sys_fork(struct trapframe *ctf, pid_t *retval) {
    struct trapframe *tf_child;
    struct proc *newp;
    int result;

    KASSERT(curproc != NULL);

    newp = proc_create_runprogram(curproc->p_name);
    if (newp == NULL) {
        return ENOMEM;
    }
#if PC_LINK
    // linking parent and child, so that child terminated on parent exit
    curproc->num_children++;

    struct proc* child;

	for (int i=0; i<MAX_CHILDREN; i++){
		child= p->children[i];
		if(child==NULL) curproc->children[i]= newp;
	}
#endif
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


    result = thread_fork(
        curthread->t_name, newp,
		call_enter_forked_process, 
		(void *)tf_child, (unsigned long)0);

  if (result){
    proc_destroy(newp);
    kfree(tf_child);
    return ENOMEM;
  }

  *retval = newp->p_pid;

  return 0;
}

// execv syscall

int sys_execv( char * progname, char * args[]){

    struct addrspace *as;
    struct vnode *v;
    vaddr_t entrypoint,stackptr, ptr_argv;
    int result;


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
        kfree(kargs);
        return result;
    }
    curproc->p_name=kargs[0];

    //open the executable file
    result = vfs_open(kargs[0],O_RDONLY,0,&v);

    if(result){
        kfree(kargs);
        return result;
    }
    //create a new address space
    as = proc_getas();
    as_destroy(as);

    as = as_create();
    if(as == NULL){
        vfs_close(v);
        kfree(kargs);
        return ENOMEM;
    }

    //switch to the new address space
    proc_setas(as);
    as_activate(); 

    //load the executable

    result = load_elf(v,&entrypoint);
    if(result){
        vfs_close(v);
        kfree(kargs);
        return result;
    }

    //done with the file
    vfs_close(v);


    //define the user stack in the address space
    result = as_define_stack(as,&stackptr);
    if(result){
        kfree(kargs);
        return result;
    }

    //copy arguments to the user stack
    result = copyout_args(kargs,num_args,&stackptr, &ptr_argv);
    if(result){
        kfree(kargs);
        return result;
    }
    kfree(kargs);
    enter_new_process(num_args,(userptr_t)ptr_argv,NULL,stackptr,entrypoint);

    //enter_new_process does not return
    panic("enter new process returned\n");
    return EINVAL;
}

/**** function to save arguments in the kernel space *****/

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

/**** auxiliary function for copyout_args *****/

void rounddown(vaddr_t* stackptr, size_t sz ){
    *stackptr= ((*stackptr)/sz)*sz;
}

/**** function to pass arguments to the user process (main) through the stack *****/

int copyout_args(char ** kargs, int num_args, vaddr_t * stackptr, vaddr_t * ptr_argv){
    int result,i;
    vaddr_t argv[num_args];

    //copy arguments to user stack in reverse order
    for (i = num_args ; i >=1; i--){

        size_t arglen = strlen(kargs[i])+1;
        //align the stack pointer to a multiple of 4 to save the pointer to the arguments strings
        *stackptr -= ROUNDUP(arglen,sizeof(void*));
        result = copyoutstr(kargs[i],(userptr_t)(*stackptr),arglen,NULL);
        if(result){
            return result;
        }
        argv[i-1] = *stackptr;
    }

    //copy argument pointers to user stack and align it as before
    *stackptr -= ROUNDUP ((num_args)*sizeof(void*),sizeof(void*));
    result = copyout(argv,(userptr_t)(*stackptr),(num_args)*sizeof(void*));
    if(result){
            return result;
    }
    *ptr_argv=*stackptr;
    rounddown(stackptr,2*sizeof(void*));    //align the stack pointer to a multiple of 8
   
    return 0;
}

