#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#define COLLECT_PROC_TIMING
#define PRIO_NUM 4
#define NULL 0

struct {
    struct spinlock lock;
    struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;


extern void forkret(void);

extern void trapret(void);

static void wakeup1(void *chan);

int set_priority(int);

void
pinit(void) {
    initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
    return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *mycpu(void) {
    int apicid, i;

    if (readeflags() & FL_IF)
        panic("mycpu called with interrupts enabled\n");

    apicid = lapicid();
    // APIC IDs are not guaranteed to be contiguous. Maybe we should have
    // a reverse map, or reserve a register to store &cpus[i].
    for (i = 0; i < ncpu; ++i) {
        if (cpus[i].apicid == apicid)
            return &cpus[i];
    }
    panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void) {
    struct cpu *c;
    struct proc *p;
    pushcli();
    c = mycpu();
    p = c->proc;
    popcli();
    return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *
allocproc(void) {
    struct proc *p;
    char *sp;

    acquire(&ptable.lock);

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if (p->state == UNUSED)
            goto found;

    release(&ptable.lock);
    return 0;

    found:
    p->state = EMBRYO;
    p->pid = nextpid++;
    //AQ
    p->ctime = ticks;
    p->stime = 0;
    p->retime = 0;
    p->rutime = 0;
    p->priority = 2;
    p->timeslice = 16;
    release(&ptable.lock);

    // Allocate kernel stack.
    if ((p->kstack = kalloc()) == 0) {
        p->state = UNUSED;
        return 0;
    }
    sp = p->kstack + KSTACKSIZE;

    // Leave room for trap frame.
    sp -= sizeof *p->tf;
    p->tf = (struct trapframe *) sp;

    // Set up new context to start executing at forkret,
    // which returns to trapret.
    sp -= 4;
    *(uint *) sp = (uint) trapret;

    sp -= sizeof *p->context;
    p->context = (struct context *) sp;
    memset(p->context, 0, sizeof *p->context);
    p->context->eip = (uint) forkret;

    return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void) {
    struct proc *p;
    extern char _binary_initcode_start[], _binary_initcode_size[];

    p = allocproc();

    initproc = p;
    if ((p->pgdir = setupkvm()) == 0)
        panic("userinit: out of memory?");
    inituvm(p->pgdir, _binary_initcode_start, (int) _binary_initcode_size);
    p->sz = PGSIZE;
    memset(p->tf, 0, sizeof(*p->tf));
    p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
    p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
    p->tf->es = p->tf->ds;
    p->tf->ss = p->tf->ds;
    p->tf->eflags = FL_IF;
    p->tf->esp = PGSIZE;
    p->tf->eip = 0;  // beginning of initcode.S

    safestrcpy(p->name, "initcode", sizeof(p->name));
    p->cwd = namei("/");

    // this assignment to p->state lets other cores
    // run this process. the acquire forces the above
    // writes to be visible, and the lock is also needed
    // because the assignment might not be atomic.
    acquire(&ptable.lock);

    p->state = RUNNABLE;
    //AQ
    //p->enter_wait_runnable_timestamp = ticks;

    release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n) {
    uint sz;
    struct proc *curproc = myproc();

    sz = curproc->sz;
    if (n > 0) {
        if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
            return -1;
    } else if (n < 0) {
        if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
            return -1;
    }
    curproc->sz = sz;
    switchuvm(curproc);
    return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void) {
    int i, pid;
    struct proc *np;
    struct proc *curproc = myproc();

    // Allocate process.
    if ((np = allocproc()) == 0) {
        return -1;
    }

    // Copy process state from proc.
    if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0) {
        kfree(np->kstack);
        np->kstack = 0;
        np->state = UNUSED;
        return -1;
    }
    np->sz = curproc->sz;
    np->parent = curproc;
    *np->tf = *curproc->tf;

    // Clear %eax so that fork returns 0 in the child.
    np->tf->eax = 0;

    for (i = 0; i < NOFILE; i++)
        if (curproc->ofile[i])
            np->ofile[i] = filedup(curproc->ofile[i]);
    np->cwd = idup(curproc->cwd);

    safestrcpy(np->name, curproc->name, sizeof(curproc->name));

    pid = np->pid;

    acquire(&ptable.lock);

    np->state = RUNNABLE;
    //AQ
    //np->enter_wait_runnable_timestamp = ticks;

    release(&ptable.lock);

    return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void) {
    struct proc *curproc = myproc();
    struct proc *p;
    int fd;

    if (curproc == initproc) {
        panic("init exiting");
    }

    // Close all open files.
    for (fd = 0; fd < NOFILE; fd++) {
        if (curproc->ofile[fd]) {
            fileclose(curproc->ofile[fd]);
            curproc->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(curproc->cwd);
    end_op();
    curproc->cwd = 0;

    acquire(&ptable.lock);

    // Parent might be sleeping in wait().
    wakeup1(curproc->parent);

    // Pass abandoned children to init.
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->parent == curproc) {
            p->parent = initproc;
            if (p->state == ZOMBIE)
                wakeup1(initproc);
        }
    }

    // Jump into the scheduler, never to return.
    curproc->state = ZOMBIE;
    //curproc->elapsed = ticks - curproc->ctime; //number of ticks since process creation until death
    sched();
    panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void) {
    struct proc *p;
    int havekids, pid;
    struct proc *curproc = myproc();

    acquire(&ptable.lock);
    for (;;) {
        // Scan through table looking for exited children.
        havekids = 0;
        for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
            if (p->parent != curproc)
                continue;
            havekids = 1;
            if (p->state == ZOMBIE) {
                // Found one.
                pid = p->pid;
                kfree(p->kstack);
                p->kstack = 0;
                freevm(p->pgdir);
                p->pid = 0;
                p->parent = 0;
                p->name[0] = 0;
                p->killed = 0;
                p->state = UNUSED;
                release(&ptable.lock);
                return pid;
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || curproc->killed) {
            release(&ptable.lock);
            return -1;
        }

        // Wait for children to exit.  (See wakeup1 call in proc_exit.)
        sleep(curproc, &ptable.lock);  //DOC: wait-sleep
    }
}

// static void print_proc_stat(struct proc *p) {
//     static int pid = 0;
//     static int prio = 0;
//     if (p->parent == 0) {
//         return;
//     }
//     if (pid != p->pid || (prio != p->priority)) {
//         cprintf("[%d,%d] ", p->pid, p->priority);
//         pid = p->pid;
//         prio = p->priority;
//     }
// }

int wait2(int *retime, int *rutime, int *stime, int *elapsed) {
	struct proc *p;
    int havekids, pid;
    struct proc *curproc = myproc();

    acquire(&ptable.lock);
    for (;;) {
        // Scan through table looking for exited children.
        havekids = 0;
        for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
            if (p->parent != curproc)
                continue;
            havekids = 1;
            if (p->state == ZOMBIE) {
                // Found one.
                pid = p->pid;
                kfree(p->kstack);
                p->kstack = 0;
                freevm(p->pgdir);
                p->pid = 0;
                p->parent = 0;
                p->name[0] = 0;
                p->killed = 0;
                p->state = UNUSED;
                //updating parameters
                *retime =  p->retime;
                *rutime = p->rutime;
                *stime = p->stime;
                *elapsed = p->elapsed;
                release(&ptable.lock);
                return pid;
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || curproc->killed) {
            release(&ptable.lock);
            return -1;
        }

        // Wait for children to exit.  (See wakeup1 call in proc_exit.)
        sleep(curproc, &ptable.lock);  //DOC: wait-sleep
    }
}
/*
//Get process from ptable. Alex HW2
struct proc* get_process( int pPriority,int index){
  //find process in ptable (%NPROC-because index is out of range we want to loop on ptable from the begining)
  struct proc *p = &ptable.proc[(index) % NPROC];
  if  (p->state != RUNNABLE || p->priority != pPriority ) 
  {
    return 0;
  }
  if (p->priority==0){
    return p; 
  } 
  if (p->timeslice>0){
    return p; 
  }
  return 0;
}

//Get process with prio.If no process with valid budget and piority=prio, update budgets. Alex HW2
struct proc* search_for_proc_with_prio(int pPriority,int idxPrios[]){
  int i;
  struct proc* p;
  int procWithBudgetZero=1;

  checkagain:
  for (i = 0; i < NPROC; i++) {
    p = get_process(pPriority,idxPrios[pPriority]+i);
    if(p!=0){
      idxPrios[pPriority] = (idxPrios[pPriority] + i +1)%NPROC;
      return p;
    }
  }

  while (procWithBudgetZero){
    for (i = 0; i < NPROC; i++) {
      p = &ptable.proc[i];
      if(p->priority==pPriority &&p->state==RUNNABLE && p->timeslice==0){
        p->timeslice = (32 / p->priority);
        if (p->timeslice < 16)
            p->timeslice = 8;
      // cprintf("\nBudget updating for Child(%d) in prio: %d\n",p->pid,p->priority);
      }
    }
    procWithBudgetZero=0;
    goto checkagain; 
  }
  return 0;
}
//Find process with the highest priority to run
//First tries to find process with priority 3 and that is RUNNABLE.
//If failed, tries to find process with priority 2 and that is RUNNABLE and so on. Alex HW2
struct proc* choose_process_to_run() {
   int idxPrios[4] = {0};
  int pPriority = 3;
  struct proc* p;
  notfound:
  p=search_for_proc_with_prio(pPriority,idxPrios);
  if (p!=0){
    return p;
  }
  if (pPriority == 0) {
    return 0;
  }
  else {
    pPriority--; 
    goto notfound;
  }
  return 0;
}*/

void updateNextEpoch(int priority, int expired[PRIO_NUM], int* exit) {
    if (!expired[priority]) {
        if (priority == 3)
            *exit = 1;
        return;
    }
    for (struct proc* p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->state == RUNNABLE && p->timeslice == 0) {
            p->timeslice = ((32 / priority) < 16) ? 8 : (32 / priority);
        }
    }
    expired[priority] = 0;
}

struct proc* getNextProcessToRun() {
    int prioIndices[PRIO_NUM] = {0};
	struct proc* p;
    int expired[PRIO_NUM] = {0, 1, 1, 1};
    int exit = 0;
	for (int priority = 3 ; priority >= 0; priority--) {
		for (int i = 0; i < NPROC; i++) {
			p = &ptable.proc[(prioIndices[priority] + i) % NPROC];
			if (p->state == RUNNABLE && p->timeslice > 0 && p->priority == priority) {
                prioIndices[priority] = (prioIndices[priority] + i + 1) % NPROC;
                return p;
			}
            if (p->state == RUNNABLE && priority == 0 && p->priority == priority)
                return p;
		}
        updateNextEpoch(priority, expired, &exit);
        if (exit)
            break; 
	}
    return NULL;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void) {
	//AQ 
    struct proc *p;
    struct cpu *c = mycpu();
    c->proc = 0;
   

    for (;;) {
        // Enable interrupts on this processor.
        sti();


        // Loop over process table looking for process to run.
        acquire(&ptable.lock);
 
        for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        	if (p->state != RUNNABLE)
        		continue;
        	struct proc *p1;
            p1 = getNextProcessToRun();
            if (p1 != NULL) {
                p = p1;
            }
            c->proc = p;
        	//print_proc_stat(p);
#ifdef COLLECT_PROC_TIMING
       		// update our stats. This has to be done exactly once every TICK.
       		p->rutime++;
            if (p->priority)
       		   p->timeslice--;
#endif //COLLECT_PROC_TIMING
        	switchuvm(p);
        	p->state = RUNNING;
        	swtch(&(c->scheduler), p->context);
        	switchkvm();
        	// Process is done running for now.
        	// It should have changed its p->state before coming back.
        	c->proc = 0;	
        }
        release(&ptable.lock);
    }
}


int set_priority(int prio) {
   struct proc *p = myproc();
   p->priority = prio;
   switch (p->priority) {
   	case 1 : 
   		p->timeslice = 32;
         return 0;
   	case 2 :
   		p->timeslice = 16;
         return 0;
   	case 3 : 
   		p->timeslice = 8;
   	   return 0;
   	default : 
   		p->timeslice = -1;
         return 0;
   }
   return 1;
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void) {
    int intena;
    struct proc *p = myproc();
    if (!holding(&ptable.lock))
        panic("sched ptable.lock");
    if (mycpu()->ncli != 1)
        panic("sched locks");
    if (p->state == RUNNING)
        panic("sched running");
    if (readeflags() & FL_IF)
        panic("sched interruptible");
    intena = mycpu()->intena;
    swtch(&p->context, mycpu()->scheduler);
    mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void) {
    acquire(&ptable.lock);  //DOC: yieldlock
    myproc()->state = RUNNABLE;
    sched();
    release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void) {
    static int first = 1;
    // Still holding ptable.lock from scheduler.
    release(&ptable.lock);

    if (first) {
        // Some initialization functions must be run in the context
        // of a regular process (e.g., they call sleep), and thus cannot
        // be run from main().
        first = 0;
        iinit(ROOTDEV);
        initlog(ROOTDEV);
    }

    // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk) {
    struct proc *p = myproc();

    if (p == 0)
        panic("sleep");

    if (lk == 0)
        panic("sleep without lk");

    // Must acquire ptable.lock in order to
    // change p->state and then call sched.
    // Once we hold ptable.lock, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup runs with ptable.lock locked),
    // so it's okay to release lk.
    if (lk != &ptable.lock) {  //DOC: sleeplock0
        acquire(&ptable.lock);  //DOC: sleeplock1
        release(lk);
    }

    // Go to sleep.
    p->chan = chan;
    p->state = SLEEPING;


    sched();

    // Tidy up.
    p->chan = 0;

    // Reacquire original lock.
    if (lk != &ptable.lock) {  //DOC: sleeplock2
        release(&ptable.lock);
        acquire(lk);
    }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan) {
    struct proc *p;

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if (p->state == SLEEPING && p->chan == chan) {
            p->state = RUNNABLE;
        }

}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan) {
    acquire(&ptable.lock);
    wakeup1(chan);
    release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid) {
    struct proc *p;

    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->pid == pid) {
            p->killed = 1;
            // Wake process from sleep if necessary.
            if (p->state == SLEEPING) {
                p->state = RUNNABLE;
                //p->stime += ticks - p->enter_wait_sleep_timestamp;
                //p->enter_wait_runnable_timestamp = ticks;
            }
            release(&ptable.lock);
            return 0;
        }
    }
    release(&ptable.lock);
    return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void) {
    static char *states[] = {
            [UNUSED]    "unused",
            [EMBRYO]    "embryo",
            [SLEEPING]  "sleep ",
            [RUNNABLE]  "runble",
            [RUNNING]   "run   ",
            [ZOMBIE]    "zombie"
    };
    int i;
    struct proc *p;
    char *state;
    uint pc[10];

    cprintf("pid \tprio \tstate \tname\n");
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->state == UNUSED)
            continue;
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
            state = states[p->state];
        else
            state = "???";
        cprintf("%d \t%d\t%s \t%s", p->pid, p->priority, state, p->name);
        if (p->state == SLEEPING) {
            getcallerpcs((uint *) p->context->ebp + 2, pc);
            for (i = 0; i < 10 && pc[i] != 0; i++)
                cprintf(" %p", pc[i]);
        }
        cprintf("\n");
    }
}

void updateFields(void) {
    for (struct proc* p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->state != ZOMBIE)
            p->elapsed++;
        switch (p->state) {
          case RUNNABLE:
            p->retime++;
            break;
          case SLEEPING:
            p->stime++;
            break;
          default: 
            break;
        }
    }
}