/* mailbox.c
 * COS 318, Fall 2019: Project 4 IPC and Process Management
 * Mailbox implementation
 */

#include "common.h"
#include "mbox.h"
#include "queue.h"
#include "sync.h"
#include "util.h"

/* A single stored message with its length */
typedef struct {
    int n_bytes;
    char body[MAX_MESSAGE_LENGTH];
} Message;

/* One message box: a bounded buffer of messages protected by a lock with two
 * condition variables: senders wait on not_full, receivers on not_empty.
 * 'usage' counts open handles. */
typedef struct {
    char name[MBOX_NAME_LENGTH];
    char in_use;        /* 0 if this table entry is free */
    unsigned int usage; /* number of open handles */
    unsigned int count; /* number of buffered messages */
    unsigned int head;  /* index of the oldest buffered message */
    Message messages[MAX_MBOX_LENGTH];
    lock_t lock;
    condition_t not_full;
    condition_t not_empty;
} MessageBox;

static MessageBox MessageBoxen[MAX_MBOXEN];

// Perform any system-startup initialization for the message boxes
void init_mbox(void) {
    int i;

    for (i = 0; i < MAX_MBOXEN; ++i) {
        MessageBoxen[i].in_use = FALSE;
        MessageBoxen[i].usage = 0;
        MessageBoxen[i].count = 0;
        MessageBoxen[i].head = 0;
    }
}

// Opens the mailbox named 'name', or creates a new message box if it doesn't
// already exist. A message box is a bounded buffer which holds up to
// MAX_MBOX_LENGTH items. If it fails because the message box table is full, it
// will return -1. Otherwise, it returns a message box id
mbox_t do_mbox_open(const char *name) {
    int i, free_box = -1;

    if (NULL == name)
        return -1;

    enter_critical();

    /* Find an existing box with this name */
    for (i = 0; i < MAX_MBOXEN; ++i) {
        if (MessageBoxen[i].in_use &&
            same_string(MessageBoxen[i].name, (char *) name)) {
            /* Someone already made it: open a second handle */
            MessageBoxen[i].usage++;
            leave_critical();
            return (mbox_t) i;
        }
        if (! MessageBoxen[i].in_use && free_box == -1)
            free_box = i;
    }

    if (free_box == -1) {
        /* The table is full */
        leave_critical();
        return -1;
    }

    /* Create a new box */
    for (i = 0; i < MBOX_NAME_LENGTH - 1 && name[i] != '\0'; ++i)
        MessageBoxen[free_box].name[i] = name[i];
    MessageBoxen[free_box].name[i] = '\0';

    MessageBoxen[free_box].in_use = TRUE;
    MessageBoxen[free_box].usage = 1;
    MessageBoxen[free_box].count = 0;
    MessageBoxen[free_box].head = 0;

    lock_init(&MessageBoxen[free_box].lock);
    condition_init(&MessageBoxen[free_box].not_full);
    condition_init(&MessageBoxen[free_box].not_empty);

    leave_critical();
    return (mbox_t) free_box;
}

// Closes a message box
void do_mbox_close(mbox_t mbox) {
    MessageBox *m;

    if (mbox < 0 || mbox >= MAX_MBOXEN)
        return;
    m = &MessageBoxen[mbox];

    enter_critical();
    ASSERT(m->in_use);
    m->usage--;
    if (m->usage == 0)
        m->in_use = FALSE;
    leave_critical();
}

// Determine if the given message box is full. Equivalently, determine if sending
// to this message box would cause a process to block
int do_mbox_is_full(mbox_t mbox) {
    MessageBox *m;

    if (mbox < 0 || mbox >= MAX_MBOXEN)
        return 1;
    m = &MessageBoxen[mbox];

    enter_critical();
    ASSERT(m->in_use);
    leave_critical();

    return m->count == MAX_MBOX_LENGTH;
}

// Enqueues a message onto a message box. If the message box is full, the process
// will block until it can add the item. You may assume that the message box ID
// has been properly opened before this call. The message is 'nbytes' bytes
// starting at 'msg'
void do_mbox_send(mbox_t mbox, void *msg, int nbytes) {
    MessageBox *m;
    Message *next;

    if (mbox < 0 || mbox >= MAX_MBOXEN)
        return;
    m = &MessageBoxen[mbox];

    lock_acquire(&m->lock);
    /* Wait for space in the bounded buffer */
    while (m->count == MAX_MBOX_LENGTH)
        condition_wait(&m->lock, &m->not_full);

    next = &m->messages[(m->head + m->count) % MAX_MBOX_LENGTH];
    next->n_bytes = nbytes > MAX_MESSAGE_LENGTH
        ? MAX_MESSAGE_LENGTH : nbytes;
    bcopy((char *) msg, next->body, next->n_bytes);
    ++m->count;

    /* A message is now available: wake one receiver.
     * condition_signal does its own enter/leave critical, so it may be
     * called with or without the lock held. */
    condition_signal(&m->not_empty);
    lock_release(&m->lock);
}

// Receives a message from the specified message box. If empty, the process will
// block until it can remove an item. You may assume that the message box has
// been properly opened before this call. The message is copied into 'msg'. No
// more than 'nbytes' bytes will be copied into this buffer; longer messages
// will be truncated
void do_mbox_recv(mbox_t mbox, void *msg, int nbytes) {
    MessageBox *m;
    Message *next;
    int n;

    if (mbox < 0 || mbox >= MAX_MBOXEN)
        return;
    m = &MessageBoxen[mbox];

    lock_acquire(&m->lock);
    /* Wait for a message in the bounded buffer */
    while (m->count == 0)
        condition_wait(&m->lock, &m->not_empty);

    next = &m->messages[m->head];
    m->head = (m->head + 1) % MAX_MBOX_LENGTH;
    --m->count;

    n = next->n_bytes > nbytes ? nbytes : next->n_bytes;
    bcopy(next->body, (char *) msg, n);

    /* Space is now available: wake one sender */
    condition_signal(&m->not_full);
    lock_release(&m->lock);
}

// Returns the number of processes that have opened but not closed this mailbox
unsigned int do_mbox_usage_count(mbox_t mbox) {
    MessageBox *m;

    if (mbox < 0 || mbox >= MAX_MBOXEN)
        return 0;
    m = &MessageBoxen[mbox];

    enter_critical();
    ASSERT(m->in_use);
    leave_critical();

    return m->usage;
}
