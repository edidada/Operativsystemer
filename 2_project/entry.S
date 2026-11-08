# entry.S
# COS 318, Fall 2019: Project 2 Non-Preemptive Kernel
# Entry points into the kernel for processes and threads

# Field offsets in struct pcb (kernel.h). Keep in sync!
#   node        : offset  0, size 8 (prev, next pointers)
#   kflags      : offset  8  saved eflags
#   kregs       : offset 12  8 saved registers (pushal layout, 32 bytes)
#   saved_esp   : offset 44  saved kernel %esp
#   started     : offset 48
.equ NODE_SIZE, 8
.equ KFLAGS, 8
.equ KREGS, 12
.equ CONTEXT_SIZE, 40           # eflags + 8 registers + %esp
.equ SAVED_ESP, 44              # KREGS + CONTEXT_SIZE - 8
.equ STARTED, 48

.data
# Temporary storage for the stack pointer while the context is saved
scratch:
    .long   0

.text
.code32

# Save registers/flags to the context area of the current pcb
#define SAVE_CONTEXT \
    movl    %esp, scratch; \
    movl    current_running, %esp; \
    leal    (KFLAGS+CONTEXT_SIZE)(%esp), %esp; \
    pushl   scratch; \
    pushal; \
    pushfl; \
    movl    scratch, %esp

# Restore registers/flags from the context area of the current pcb
#define RESTORE_CONTEXT \
    movl    current_running, %esp; \
    leal    KFLAGS(%esp), %esp; \
    popfl; \
    popal; \
    popl    %esp

# Called (as an ordinary function) before switching tasks
# 1. Save the context of the current task if it has ever run
# 2. Let scheduler() pick the next task
# 3. Restore the context of the new current task and return to it
.globl  scheduler_entry
scheduler_entry:
    # A task that has never been scheduled has no context worth saving;
    # scheduler() will bootstrap it from the initializing values written by
    # the kernel.
    movl    current_running, %eax
    cmpl    $0, STARTED(%eax)
    je      1f
    SAVE_CONTEXT
1:
    call    scheduler
    RESTORE_CONTEXT
    ret

# System call entry (yield/exit), called from processes and threads via
# the pointer stored at ENTRY_POINT. The syscall number is the first
# argument, i.e. at 4(%esp) on entry.
.globl  kernel_entry
kernel_entry:
    movl    4(%esp), %eax
    movl    %eax, sys_arg
    movl    current_running, %eax
    cmpl    $0, STARTED(%eax)
    je      3f
    SAVE_CONTEXT
3:
    pushl   sys_arg
    call    syscall_helper
    addl    $4, %esp
    RESTORE_CONTEXT
    ret

.data
# Holds the syscall number across the (possible) switch
sys_arg:
    .long   0
