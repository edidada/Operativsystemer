/* kernel.h: definitions used by kernel code */

#ifndef KERNEL_H
#define KERNEL_H

#include "common.h"
#include "queue.h"

/* ENTRY_POINT points to a location that holds a pointer to kernel_entry */
#define ENTRY_POINT ((void (**)(int)) 0x0f00)

/* System call numbers */
enum {
    SYSCALL_YIELD,
    SYSCALL_EXIT,
};

/* All stacks should be STACK_SIZE bytes large
 * The first stack should be placed at location STACK_MIN
 * Only memory below STACK_MAX should be used for stacks
 */
enum {
    STACK_MIN = 0x10000,
    STACK_SIZE = 0x1000,
    STACK_MAX = 0x20000,
};

typedef enum {
    READY,      /* runnable, either running or in the ready queue */
    BLOCKED,    /* waiting to be unblocked (not in the ready queue) */
    EXITED      /* the task has called do_exit() */
} status_t;

/* The layout of the first four fields is fixed; entry.S depends on it.
 *
 * kflags and kregs + saved_esp form the 40 byte saved context written by
 * SAVE_CONTEXT / read by RESTORE_CONTEXT in entry.S:
 *   pcb + KFLAGS      : saved eflags
 *   pcb + KREGS(12)   : 8 saved registers (as pushal lays them out)
 *   pcb + SAVED_ESP   : kernel %esp at save time (and first-run %esp)
 */
typedef struct pcb {
    node_t node;         /* linkage for queues; must be first (offset 0) */
    uint32_t kflags;     /* offset 8 */
    uint32_t kregs[8];   /* offset 12 */
    uint32_t saved_esp;  /* offset 44 */
    /* The fields below are only touched from C */
    bool_t started;      /* has this task been scheduled at least once */
    status_t status;
    uint32_t entry_point;
} pcb_t;

/* The task currently running */
extern pcb_t *current_running;

void kernel_entry(int fn);

#endif                          /* KERNEL_H */
