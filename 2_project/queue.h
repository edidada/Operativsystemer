/* queue.h
 * COS 318, Fall 2019: Project 2 Non-Preemptive Kernel
 * Definitions and types for queue data structure
 */

#ifndef QUEUE_H
#define QUEUE_H

/* Doubly linked, circular list node. A queue is an empty node used as the
 * list head; nodes are embedded in whatever structure is queued (e.g. the
 * PCB), so enqueueing is just a relink of two pointers. */
typedef struct node {
    struct node *prev, *next;
} node_t;

void queue_init(node_t *queue);
void enqueue(node_t *queue, node_t *item);
node_t *dequeue(node_t *queue);
int is_empty(node_t *queue);
node_t *peek(node_t *queue);

#endif
