// Code for manipulating stack locations.
//
// Copyright (C) 2009  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "biosvar.h" // get_ebda_seg
#include "util.h" // dprintf
#include "bregs.h" // CR0_PE

static inline u32 getcr0() {
    u32 cr0;
    asm("movl %%cr0, %0" : "=r"(cr0));
    return cr0;
}
static inline void sgdt(struct descloc_s *desc) {
    asm("sgdtl %0" : "=m"(*desc));
}
static inline void lgdt(struct descloc_s *desc) {
    asm("lgdtl %0" : : "m"(*desc) : "memory");
}

// Call a 32bit SeaBIOS function from a 16bit SeaBIOS function.
static inline int
call32(void *func)
{
    ASSERT16();
    u32 cr0 = getcr0();
    if (cr0 & CR0_PE)
        // Called in 16bit protected mode?!
        return -1;

    // Backup cmos index register and disable nmi
    u8 cmosindex = inb(PORT_CMOS_INDEX);
    outb(cmosindex | NMI_DISABLE_BIT, PORT_CMOS_INDEX);
    inb(PORT_CMOS_DATA);

    // Backup fs/gs and gdt
    u16 fs = GET_SEG(FS), gs = GET_SEG(GS);
    struct descloc_s gdt;
    sgdt(&gdt);

    func -= BUILD_BIOS_ADDR;
    u32 bkup_ss, bkup_esp;
    asm volatile(
        // Backup ss/esp / set esp to flat stack location
        "  movl %%ss, %0\n"
        "  movl %%esp, %1\n"
        "  shll $4, %0\n"
        "  addl %0, %%esp\n"
        "  movl %%ss, %0\n"

        // Transition to 32bit mode, call yield_preempt, return to 16bit
        "  pushl $(" __stringify(BUILD_BIOS_ADDR) " + 1f)\n"
        "  jmp transition32\n"
        "  .code32\n"
        "1:calll %2\n"
        "  pushl $2f\n"
        "  jmp transition16big\n"

        // Restore ds/ss/esp
        "  .code16gcc\n"
        "2:movl %0, %%ds\n"
        "  movl %0, %%ss\n"
        "  movl %1, %%esp\n"
        : "=&r" (bkup_ss), "=&r" (bkup_esp)
        : "m" (*(u8*)func)
        : "eax", "ecx", "edx", "cc", "memory");

    // Restore gdt and fs/gs
    lgdt(&gdt);
    SET_SEG(FS, fs);
    SET_SEG(GS, gs);

    // Restore cmos index register
    outb(cmosindex, PORT_CMOS_INDEX);
    inb(PORT_CMOS_DATA);
    return 0;
}


/****************************************************************
 * Stack in EBDA
 ****************************************************************/

// Switch to the extra stack in ebda and call a function.
inline u32
stack_hop(u32 eax, u32 edx, u32 ecx, void *func)
{
    ASSERT16();
    u16 ebda_seg = get_ebda_seg(), bkup_ss;
    u32 bkup_esp;
    asm volatile(
        // Backup current %ss/%esp values.
        "movw %%ss, %w3\n"
        "movl %%esp, %4\n"
        // Copy ebda seg to %ds/%ss and set %esp
        "movw %w6, %%ds\n"
        "movw %w6, %%ss\n"
        "movl %5, %%esp\n"
        // Call func
        "calll %7\n"
        // Restore segments and stack
        "movw %w3, %%ds\n"
        "movw %w3, %%ss\n"
        "movl %4, %%esp"
        : "+a" (eax), "+d" (edx), "+c" (ecx), "=&r" (bkup_ss), "=&r" (bkup_esp)
        : "i" (EBDA_OFFSET_TOP_STACK), "r" (ebda_seg), "m" (*(u8*)func)
        : "cc", "memory");
    return eax;
}


/****************************************************************
 * Threads
 ****************************************************************/

#define THREADSTACKSIZE 4096

struct thread_info {
    struct thread_info *next;
    void *stackpos;
};

struct thread_info VAR16VISIBLE MainThread;
int VAR16VISIBLE CanPreempt;

void
thread_setup()
{
    MainThread.next = &MainThread;
    MainThread.stackpos = NULL;
    CanPreempt = 0;
}

// Return the 'struct thread_info' for the currently running thread.
struct thread_info *
getCurThread()
{
    u32 esp = getesp();
    if (esp <= BUILD_STACK_ADDR)
        return &MainThread;
    return (void*)ALIGN_DOWN(esp, THREADSTACKSIZE);
}

// Switch to next thread stack.
static void
switch_next(struct thread_info *cur)
{
    struct thread_info *next = cur->next;
    asm volatile(
        "  pushl $1f\n"                 // store return pc
        "  pushl %%ebp\n"               // backup %ebp
        "  movl %%esp, 4(%%eax)\n"      // cur->stackpos = %esp
        "  movl 4(%%ecx), %%esp\n"      // %esp = next->stackpos
        "  popl %%ebp\n"                // restore %ebp
        "  retl\n"                      // restore pc
        "1:\n"
        : "+a"(cur), "+c"(next)
        :
        : "ebx", "edx", "esi", "edi", "cc", "memory");
}

// Briefly permit irqs to occur.
void
yield()
{
    if (MODE16 || !CONFIG_THREADS) {
        // Just directly check irqs.
        check_irqs();
        return;
    }
    struct thread_info *cur = getCurThread();
    if (cur == &MainThread)
        // Permit irqs to fire
        check_irqs();

    // Switch to the next thread
    switch_next(cur);
}

// Last thing called from a thread (called on "next" stack).
static void
__end_thread(struct thread_info *old)
{
    struct thread_info *pos = &MainThread;
    while (pos->next != old)
        pos = pos->next;
    pos->next = old->next;
    free(old);
    dprintf(DEBUG_thread, "\\%08x/ End thread\n", (u32)old);
}

// Create a new thread and start executing 'func' in it.
void
run_thread(void (*func)(void*), void *data)
{
    ASSERT32();
    if (! CONFIG_THREADS)
        goto fail;
    struct thread_info *thread;
    thread = memalign_tmphigh(THREADSTACKSIZE, THREADSTACKSIZE);
    if (!thread)
        goto fail;

    thread->stackpos = (void*)thread + THREADSTACKSIZE;
    struct thread_info *cur = getCurThread();
    thread->next = cur->next;
    cur->next = thread;

    dprintf(DEBUG_thread, "/%08x\\ Start thread\n", (u32)thread);
    asm volatile(
        // Start thread
        "  pushl $1f\n"                 // store return pc
        "  pushl %%ebp\n"               // backup %ebp
        "  movl %%esp, 4(%%edx)\n"      // cur->stackpos = %esp
        "  movl 4(%%ebx), %%esp\n"      // %esp = thread->stackpos
        "  calll *%%ecx\n"              // Call func

        // End thread
        "  movl (%%ebx), %%ecx\n"       // %ecx = thread->next
        "  movl 4(%%ecx), %%esp\n"      // %esp = next->stackpos
        "  movl %%ebx, %%eax\n"
        "  calll %4\n"                  // call __end_thread(thread)
        "  popl %%ebp\n"                // restore %ebp
        "  retl\n"                      // restore pc
        "1:\n"
        : "+a"(data), "+c"(func), "+b"(thread), "+d"(cur)
        : "m"(*(u8*)__end_thread)
        : "esi", "edi", "cc", "memory");
    return;

fail:
    func(data);
}

// Wait for all threads (other than the main thread) to complete.
void
wait_threads()
{
    ASSERT32();
    if (! CONFIG_THREADS)
        return;
    while (MainThread.next != &MainThread)
        yield();
}


/****************************************************************
 * Thread preemption
 ****************************************************************/

static u32 PreemptCount;

// Turn on RTC irqs and arrange for them to check the 32bit threads.
void
start_preempt()
{
    if (! CONFIG_THREADS || ! CONFIG_THREAD_OPTIONROMS)
        return;
    CanPreempt = 1;
    PreemptCount = 0;
    useRTC();
}

// Turn off RTC irqs / stop checking for thread execution.
void
finish_preempt()
{
    if (! CONFIG_THREADS || ! CONFIG_THREAD_OPTIONROMS)
        return;
    CanPreempt = 0;
    releaseRTC();
    dprintf(1, "Done preempt - %d checks\n", PreemptCount);
}

extern void yield_preempt();
#if !MODE16
// Try to execute 32bit threads.
void VISIBLE32
yield_preempt()
{
    PreemptCount++;
    switch_next(&MainThread);
}
#endif

// 16bit code that checks if threads are pending and executes them if so.
void
check_preempt()
{
    if (! CONFIG_THREADS || ! CONFIG_THREAD_OPTIONROMS
        || !GET_GLOBAL(CanPreempt)
        || GET_GLOBAL(MainThread.next) == &MainThread)
        return;

    call32(yield_preempt);
}
