#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <mem.h>
#include <mm/types.h>
#include <mm/heap.h>
#include <mm/frame.h>
#include <mm/paging.h>
#include <mm/hhdm.h>
#include <logging/printk.h>
#include <multitasking/sched.h>
#include <multitasking/thread.h>
#include <auxv.h>

#define KERNEL_CS 0x08
#define KERNEL_SS 0x10
#define USER_CS   0x23
#define USER_SS   0x1B

#define USER_STACK_BASE  0x70000000000ULL
#define USER_STACK_PAGES 4
#define USER_STACK_SIZE  (USER_STACK_PAGES * 0x1000)

extern void      thread_trampoline();
extern uintptr_t offset;

uint16_t next_tid = 0;

struct stack_alloc {
    void *raw;
    void *aligned;
};

static struct stack_alloc alloc_aligned_stack(size_t size) {
    struct stack_alloc stack = {0};

    size_t size_pages = (size + 0xFFF) & ~0xFFFULL;

    uintptr_t raw = (uintptr_t)kmalloc(size_pages + 0x1000);
    if (!raw)
        return stack;

    uintptr_t aligned = (raw + 0xFFF) & ~0xFFFULL;

    stack.raw     = (void *)raw;
    stack.aligned = (void *)aligned;
    return stack;
}

static uint64_t alloc_user_stack(uint64_t *pml4) {
    const uint64_t flags = ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW |
                           ENTRY_FLAG_NX      | ENTRY_FLAG_USER;

    for (uint64_t i = 0; i < USER_STACK_PAGES; i++) {
        paddr frame = frame_alloc();
        vaddr virt  = USER_STACK_BASE + (i * 0x1000);
        paging_map_page(pml4, virt, frame, 0x1000, flags);
    }

    return USER_STACK_BASE + USER_STACK_SIZE;
}

static void log_frame_check(struct tcb *tcb) {
    uint64_t *check = (uint64_t *)tcb->rsp;

    log_debug("FRAME CHECK tid=%d tcb=%llX tcb->rsp=%llX\n",
              tcb->tid, (void *)tcb, tcb->rsp);

    log_debug("  [0..14]=zero-regs  r15=%llX r14=%llX r13=%llX r12=%llX r11=%llX\n",
              check[0], check[1], check[2], check[3], check[4]);
    log_debug("  r10=%llX r9=%llX r8=%llX rbp=%llX rdi=%llX\n",
              check[5], check[6], check[7], check[8], check[9]);
    log_debug("  rsi=%llX rdx=%llX rcx=%llX rbx=%llX rax=%llX\n",
              check[10], check[11], check[12], check[13], check[14]);

    log_debug("  [15]=%llX (want entry)\n", check[15]);
    log_debug("  [16]=%llX (want USER_CS=%X)\n", check[16], USER_CS);
    log_debug("  [17]=%llX (want RFLAGS=0x202)\n", check[17]);
    log_debug("  [18]=%llX (want user_rsp)\n", check[18]);
    log_debug("  [19]=%llX (want USER_SS=%X)\n", check[19], USER_SS);

    log_debug("  stack_base=%llX kstack_top_calc=%llX offset_of_rsp_from_top=%lld\n",
              tcb->stack_base,
              (void *)((uint8_t *)tcb->stack_base + KERNEL_STACK_SIZE),
              (int64_t)(((uint8_t *)tcb->stack_base + KERNEL_STACK_SIZE) - (uint8_t *)tcb->rsp));

    log_debug("  sizeof(struct tcb)=%lu tcb_addr=%llX tcb_end=%llX rsp_inside_tcb=%d\n",
              (unsigned long)sizeof(struct tcb), (void *)tcb, (void *)((uint8_t *)tcb + sizeof(struct tcb)),
              ((uint8_t *)tcb->rsp >= (uint8_t *)tcb &&
               (uint8_t *)tcb->rsp <  (uint8_t *)tcb + sizeof(struct tcb)));
}

uint64_t setup_stack(uint8_t *stack_base, uint64_t stack_size, void *entry) {
    uint64_t *stack_top = (uint64_t *)(stack_base + stack_size);
    uint64_t *rsp       = stack_top;

    *--rsp = KERNEL_SS;
    *--rsp = (uint64_t)stack_top;
    *--rsp = 0x202;
    *--rsp = KERNEL_CS;
    *--rsp = (uint64_t)thread_trampoline;

    for (int i = 0; i < 15; i++)
        *--rsp = 0;

    rsp[13] = (uint64_t)entry;

    log_debug("setup_stack: rsp=%llX [13]=%llX (want entry=%llX)\n",
              (void *)rsp, rsp[13], entry);

    return (uint64_t)rsp;
}

struct tcb *create_user_thread(uint64_t entry, paddr cr3) {
    uint64_t *pml4      = (uint64_t *)(offset + cr3);
    uint64_t user_stack = alloc_user_stack(pml4);

    struct stack_alloc kstack = alloc_aligned_stack(KERNEL_STACK_SIZE);
    if (!kstack.aligned) {
        log_error("create_user_thread: alloc_aligned_stack failed\n");
        return NULL;
    }

    struct tcb *tcb = kmalloc(sizeof(struct tcb));
    if (!tcb) {
        log_error("create_user_thread: tcb kmalloc failed\n");
        kfree(kstack.raw);
        return NULL;
    }

    log_debug("create_user_thread: kstack.raw=%llX kstack.aligned=%llX tcb=%llX (delta=%lld)\n",
              kstack.raw, kstack.aligned, (void *)tcb,
              (int64_t)((uint8_t *)tcb - (uint8_t *)kstack.aligned));

    uint64_t *rsp = (uint64_t *)((uint8_t *)kstack.aligned + KERNEL_STACK_SIZE);

    *--rsp = USER_SS;
    *--rsp = user_stack;
    *--rsp = 0x202;
    *--rsp = USER_CS;
    *--rsp = entry;

    for (int i = 0; i < 15; i++)
        *--rsp = 0;

    log_debug("User thread %d cr3: %llX entry: %llX ustack: %llX\n",
              next_tid, cr3, entry, user_stack);

    tcb->tid         = next_tid++;
    tcb->parent      = NULL;
    tcb->cr3         = cr3;
    tcb->state       = ready;
    tcb->stack_base  = kstack.raw;
    tcb->ustack_base = NULL;
    tcb->entry       = (void *)entry;
    tcb->rsp         = (uint64_t)rsp;

    log_frame_check(tcb);

    enqueue(tcb);
    return tcb;
}

struct tcb *create_user_thread_with_stack(uint64_t entry, paddr cr3,
                                           char **argv, char **envp,
                                           const elf_load_info_t *info) {
    uint64_t *pml4      = (uint64_t *)(offset + cr3);
    uint64_t ustack_top = alloc_user_stack(pml4);
    uint64_t user_rsp   = ustack_top;

    log_debug("create_user_thread_with_stack: ustack_top=%llX\n", ustack_top);

    if (info) {
        void *rsp = build_user_stack(pml4, ustack_top, USER_STACK_BASE,
                                     argv, envp, info);
        if (!rsp) {
            log_error("create_user_thread_with_stack: build_user_stack failed\n");
            return NULL;
        }
        user_rsp = (uint64_t)rsp;
        log_debug("create_user_thread_with_stack: build_user_stack returned %llX\n", user_rsp);
    }

    struct stack_alloc kstack = alloc_aligned_stack(KERNEL_STACK_SIZE);
    if (!kstack.aligned) {
        log_error("create_user_thread_with_stack: alloc_aligned_stack failed\n");
        return NULL;
    }

    struct tcb *tcb = kmalloc(sizeof(struct tcb));
    if (!tcb) {
        log_error("create_user_thread_with_stack: tcb kmalloc failed\n");
        kfree(kstack.raw);
        return NULL;
    }

    log_debug("create_user_thread_with_stack: kstack.raw=%llX kstack.aligned=%llX tcb=%llX (delta=%lld)\n",
              kstack.raw, kstack.aligned, (void *)tcb,
              (int64_t)((uint8_t *)tcb - (uint8_t *)kstack.aligned));

    uint64_t *rsp = (uint64_t *)((uint8_t *)kstack.aligned + KERNEL_STACK_SIZE);

    *--rsp = USER_SS;
    *--rsp = user_rsp;
    *--rsp = 0x202;
    *--rsp = USER_CS;
    *--rsp = entry;

    for (int i = 0; i < 15; i++)
        *--rsp = 0;

    log_debug("User thread %d cr3: %llX entry: %llX ustack: %llX\n",
              next_tid, cr3, entry, user_rsp);

    tcb->tid         = next_tid++;
    tcb->parent      = NULL;
    tcb->cr3         = cr3;
    tcb->state       = ready;
    tcb->stack_base  = kstack.raw;
    tcb->ustack_base = NULL;
    tcb->entry       = (void *)entry;
    tcb->rsp         = (uint64_t)rsp;

    log_frame_check(tcb);

    enqueue(tcb);
    return tcb;
}

struct tcb *create_thread(void *entry, paddr cr3) {
    struct stack_alloc kstack = alloc_aligned_stack(KERNEL_STACK_SIZE);
    if (!kstack.aligned) {
        log_error("create_thread: alloc_aligned_stack failed\n");
        return NULL;
    }

    struct tcb *tcb = kmalloc(sizeof(struct tcb));
    if (!tcb) {
        log_error("create_thread: tcb kmalloc failed\n");
        kfree(kstack.raw);
        return NULL;
    }

    log_debug("Thread %d cr3: %llX\n", next_tid, cr3);

    tcb->tid         = next_tid++;
    tcb->parent      = NULL;
    tcb->cr3         = cr3;
    tcb->state       = ready;
    tcb->stack_base  = kstack.raw;
    tcb->ustack_base = NULL;
    tcb->entry       = entry;
    tcb->rsp         = setup_stack(
                           (uint8_t *)kstack.aligned,
                           KERNEL_STACK_SIZE,
                           entry);

    log_frame_check(tcb);

    enqueue(tcb);
    return tcb;
}

struct tcb *thread_fork(struct tcb *parent, paddr cr3) {
    if (!parent) {
        log_error("thread_fork: NULL parent\n");
        return NULL;
    }

    struct stack_alloc kstack = alloc_aligned_stack(KERNEL_STACK_SIZE);
    if (!kstack.aligned) {
        log_error("thread_fork: alloc_aligned_stack failed\n");
        return NULL;
    }

    struct tcb *child = kmalloc(sizeof(struct tcb));
    if (!child) {
        log_error("thread_fork: tcb kmalloc failed\n");
        kfree(kstack.raw);
        return NULL;
    }

    memcpy(child, parent, sizeof(struct tcb));

    child->tid        = next_tid++;
    child->parent     = parent->parent;
    child->cr3        = cr3;
    child->state      = ready;
    child->stack_base = kstack.raw;

    uintptr_t parent_stack =
        ((uintptr_t)parent->stack_base + 0xFFF) & ~0xFFFULL;

    uintptr_t child_stack =
        (uintptr_t)kstack.aligned;

    memcpy(
        (void *)child_stack,
        (void *)parent_stack,
        KERNEL_STACK_SIZE
    );

    uintptr_t rsp_offset =
        parent->rsp - parent_stack;

    child->rsp = child_stack + rsp_offset;

    uint64_t *regs = (uint64_t *)child->rsp;
    regs[14] = 0;

    log_debug("thread_fork: parent_tid=%d child_tid=%d parent_rsp=%llX child_rsp=%llX rsp_offset=%llX\n",
              parent->tid, child->tid, parent->rsp, child->rsp, (uint64_t)rsp_offset);

    log_frame_check(child);

    return child;
}

void thread_destroy(struct tcb *thread) {
    if (!thread)
        return;
    log_info("Freeing thread %d\n", thread->tid);
    kfree(thread->ustack_base);
    kfree(thread->stack_base);
    frame_free(thread->cr3);
    kfree(thread);
}

void handle_ret(int64_t code) {
    __asm__ volatile("cli");
    current_thread->state = dead;
    log_info("Thread %d exited with code %ld\n", current_thread->tid, code);
    __asm__ volatile("int $32");
    __builtin_unreachable();
}