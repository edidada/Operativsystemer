/* queue.c
 * COS 318, Fall 2019: Project 2 Non-Preemptive Kernel
 * Queue data structure
 *
 * Circular doubly linked list with a sentinel head node.
 * FIFO: dequeue() returns the oldest enqueued item.
 */

#include "common.h"
#include "queue.h"

void queue_init(node_t *queue) {
    queue->prev = queue->next = queue;
}

void enqueue(node_t *queue, node_t *item) {
    item->prev = queue->prev;
    item->next = queue;
    item->prev->next = item;
    item->next->prev = item;
}

node_t *dequeue(node_t *queue) {
    node_t *item;

    item = queue->next;
    if (item == queue)
        return NULL;    /* the queue is empty */
    item->prev->next = item->next;
    item->next->prev = item->prev;
    /* Not strictly necessary, but it makes stale pointers obvious */
    item->next = item->prev = item;
    return item;
}

int is_empty(node_t *queue) {
    return queue->next == queue;
}

node_t *peek(node_t *queue) {
    return (queue->next == queue) ? NULL : queue->next;
}
