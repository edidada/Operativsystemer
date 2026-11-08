/* scheduler.c
 * COS 318, Fall 2019: Project 2 Non-Preemptive Kernel
 * A non-preemptive scheduler
 */

#include "common.h"
#include "kernel.h"
#include "queue.h"
#include "scheduler.h"
#include "util.h"

int scheduler_count;

/* Ready queue: tasks waiting to be scheduled (round-robin) */
node_t ready_queue;

/* Blocked queue used by the generic block()/unblock() API.
 * Locks keep their own waiter queues so that wake-ups stay FIFO per lock. */
node_t blocked_queue;

void scheduler(void) {
    pcb_t *next;

    ++scheduler_count;

    /* A task that is still runnable goes back at the end of the ready
     * queue; exited and blocked tasks are not rescheduled. */
    if (current_running->status == READY) {
        enqueue(&ready_queue, (node_t *) current_running);
    } else if (current_running->status == EXITED ||
               current_running->status == BLOCKED) {
        /* nothing to requeue */
    } else {
        ASSERT(0);
    }

    next = (pcb_t *) dequeue(&ready_queue);
    if (NULL == next) {
        /* Every task has exited or is blocked with nobody left to unblock:
         * the system is deadlocked, so stop here. */
        HALT("scheduler: no runnable tasks");
    }
    current_running = next;
    next->started = TRUE;
}

void do_yield(void) {
    /* Re-entering scheduler_entry() saves our context, lets scheduler()
     * put us at the end of the ready queue and hands the CPU to another
     * task. When this task is scheduled again, scheduler_entry() returns
     * to the instruction following this call. */
    scheduler_entry();
}

void do_exit(void) {
    current_running->status = EXITED;
    scheduler_entry();
    /* scheduler_entry() must never return for a task that exited */
}

void block(void) {
    current_running->status = BLOCKED;
    enqueue(&blocked_queue, (node_t *) current_running);
    scheduler_entry();
}

void unblock(void) {
    pcb_t *task;

    task = (pcb_t *) dequeue(&blocked_queue);
    if (NULL != task) {
        task->status = READY;
        enqueue(&ready_queue, (node_t *) task);
    }
}

bool_t blocked_tasks(void) {
    return !is_empty(&blocked_queue) ? TRUE : FALSE;
}
