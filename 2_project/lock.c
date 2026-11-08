/* lock.c: mutual exclusion
 * If SPIN is false, lock_acquire() should block the calling process until its request can be satisfied
 * Your solution must satisfy the FIFO fairness property
 */

#include "common.h"
#include "lock.h"
#include "queue.h"
#include "scheduler.h"
#include "util.h"

enum {
    SPIN = FALSE,
};

void lock_init(lock_t *l)
{
    if (SPIN) {
        l->status = UNLOCKED;
    } else {
        l->status = UNLOCKED;
        queue_init(&l->waiters);
    }
}

void lock_acquire(lock_t *l)
{
    if (SPIN) {
        while (LOCKED == l->status)
            do_yield();
        l->status = LOCKED;
    } else {
        /* Uncontended: grab the lock */
        if (UNLOCKED == l->status) {
            l->status = LOCKED;
            return;
        }
        /* Contended: FIFO wait until the current owner releases the lock
         * and hands it over. The task stays blocked inside lock_acquire();
         * when it resumes, it owns the lock. */
        enqueue(&l->waiters, (node_t *) current_running);
        current_running->status = BLOCKED;
        scheduler_entry();
        /* On resume from lock_release() the lock is ours */
    }
}

void lock_release(lock_t *l)
{
    if (SPIN) {
        l->status = UNLOCKED;
    } else {
        node_t *waiter;
        pcb_t *task;

        /* Hand the lock directly to the head of the FIFO waiter queue */
        waiter = dequeue(&l->waiters);
        task = (pcb_t *) waiter;
        if (NULL != task) {
            task->status = READY;
            enqueue(&ready_queue, &task->node);
            /* The lock stays LOCKED: ownership passed to the waiter */
        } else {
            l->status = UNLOCKED;
        }
    }
}
