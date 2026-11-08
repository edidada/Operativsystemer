/* kernel.c */

#include "common.h"
#include "kernel.h"
#include "queue.h"
#include "scheduler.h"
#include "th.h"
#include "util.h"

#include "tasks.c"

pcb_t *current_running;

/* One pcb per task declared in tasks.c */
static pcb_t pcbs[NUM_TASKS];

/*
 * Tasks need to run from a defined stack, so every task gets a first-run
 * trampoline: the pcb's saved esp points to a word holding the task's entry
 * point, with do_exit right below it. On the very first restore the `ret`
 * in entry.S jumps to the task's entry point; if the task's C function ever
 * simply returns, the return address on the stack sends it to do_exit().
 */
static void init_pcb(pcb_t *p, struct task_info *task, int stack_num)
{
    uint32_t top;    /* top of the task's stack */
    uint32_t *sp;

    p->started = FALSE;
    p->status = READY;
    p->entry_point = task->entry_point;
    p->kflags = 0x202;              /* eflags: IF set, reserved bit 1 set */
    bzero((char *) p->kregs, sizeof p->kregs);

    /* All stacks are STACK_SIZE bytes; stack "stack_num" ends at
     * STACK_MIN + (stack_num + 1) * STACK_SIZE, below STACK_MAX */
    top = STACK_MIN + (stack_num + 1) * STACK_SIZE;
    sp = (uint32_t *) (top - 8);
    sp[0] = task->entry_point;      /* popped by `ret` on first restore */
    sp[1] = (uint32_t) &do_exit;    /* return path if the task function ends */
    p->saved_esp = (uint32_t) sp;
}

/* Dispatch a syscall for the current task. Called from kernel_entry() in
 * entry.S with the context of the current task already saved. */
void syscall_helper(int fn)
{
    switch (fn) {
    case SYSCALL_YIELD:
        /* The context is already saved; just switch tasks.
         * scheduler() re-enqueues us (status == READY) so we run again. */
        scheduler();
        break;
    case SYSCALL_EXIT:
        current_running->status = EXITED;
        scheduler();
        break;
    default:
        HALT("kernel_entry: unknown syscall");
    }
}

/* This function is the entry point for the kernel
 * It must be the first function in the file
 */
void _start(void)
{
    int i;

    /* Set up the single entry-point for system calls */
    *ENTRY_POINT = &kernel_entry;

    clear_screen(0, 0, 80, 25);

    /* Initialize the pcbs and the ready queue */
    queue_init(&ready_queue);
    queue_init(&blocked_queue);
    for (i = 0; i < NUM_TASKS; i++) {
        init_pcb(&pcbs[i], task[i], i);
        enqueue(&ready_queue, &pcbs[i].node);
    }
    /* The first task is "running" but has not been scheduled yet; its
     * first-run trampoline is still intact and scheduler() will give it
     * more than its fair share of turns later. */
    current_running = &pcbs[0];

    /* Schedule the first task */
    scheduler_count = 0;
    scheduler_entry();

    /* We shouldn't ever get here */
    ASSERT(0);
}
