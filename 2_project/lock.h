/* lock.h
 * COS 318, Fall 2019: Project 2 Non-Preemptive Kernel
 * Definitions and types for basic mutual exclusion
 */

#ifndef LOCK_H
#define LOCK_H

#include "queue.h"

typedef struct {
    enum {
        UNLOCKED,
        LOCKED,
    } status;
    /* Tasks waiting for the lock, kept in FIFO order so that lock handover
     * is fair (the first waiter always gets the lock next) */
    node_t waiters;
} lock_t;

void lock_init(lock_t *);
void lock_acquire(lock_t *);
void lock_release(lock_t *);

#endif
