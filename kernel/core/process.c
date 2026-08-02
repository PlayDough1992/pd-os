/* ============================================================================
 * PD-Kernel  —  Process Management  (Phase 10)
 *
 * Provides:
 *   proc_init()      — register the boot thread as pid 0
 *   proc_create()    — spawn a new kernel-mode task
 *   sched_irq()      — PIT IRQ0 hook; saves/restores ESP for context switch
 *   proc_exit()      — terminate current task
 *   ps / kill helpers
 * ============================================================================ */

#include "process.h"
#include "kheap.h"
#include "pic.h"
#include "pit.h"
#include "kernel.h"

/* ---- private helpers ----------------------------------------------------- */

static void p_memset(void *dst, uint8_t val, uint32_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = val;
}

static void p_strncpy(char *dst, const char *src, int n)
{
    int i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ---- globals ------------------------------------------------------------- */

static pcb_t    g_procs[PROC_MAX];
static int      g_current  = 0;
static uint32_t g_next_pid = 0;

/* ---- proc_init ----------------------------------------------------------- */

void proc_init(void)
{
    int i;
    for (i = 0; i < PROC_MAX; i++) {
        g_procs[i].state      = PROC_UNUSED;
        g_procs[i].stack_base = 0;
    }

    /* pid 0 = the currently-running boot thread (kernel/shell) */
    g_procs[0].pid         = g_next_pid++;
    g_procs[0].state       = PROC_RUNNING;
    g_procs[0].ticks_rem   = PROC_QUANTUM;
    g_procs[0].ticks_total = 0;
    g_procs[0].stack_base  = 0;   /* uses the existing kernel stack */
    g_procs[0].saved_esp   = 0;   /* filled on the first IRQ0 */
    p_strncpy(g_procs[0].name, "kernel/shell", sizeof(g_procs[0].name));

    g_current = 0;
}

/* ---- sched_irq ----------------------------------------------------------- */
/*
 * Called directly from irq0_preempt (sched_entry.asm) with the current ESP
 * pointing at the top of the saved interrupt frame (i.e. the saved DS slot).
 *
 * Returns the ESP to switch to:
 *   - same value → stay on current task
 *   - different  → switch to next task's saved frame
 *
 * Also ACKs the PIC and increments the PIT tick counter.
 */
uint32_t sched_irq(uint32_t current_esp)
{
    int next, tries;

    /* ACK PIC and tick the system clock */
    pic_send_eoi(0);
    pit_handler();

    /* Save current task's ESP */
    g_procs[g_current].saved_esp = current_esp;
    g_procs[g_current].ticks_total++;
    g_procs[g_current].ticks_rem--;

    /* Find next RUNNABLE task (skip SLEEPING and UNUSED/DEAD).
     * Also switch immediately if current task used all its quantum. */
    if (g_procs[g_current].ticks_rem > 0 &&
        g_procs[g_current].state != PROC_SLEEPING)
        return current_esp;   /* stay in current task */

    /* Round-robin: find next RUNNABLE task */
    next = (g_current + 1) % PROC_MAX;
    for (tries = 0; tries < PROC_MAX; tries++) {
        proc_state_t s = g_procs[next].state;
        if (s == PROC_RUNNABLE || s == PROC_RUNNING)
            break;
        next = (next + 1) % PROC_MAX;
    }

    if (next == g_current) {
        /* No other runnable task — keep running (reset quantum) */
        if (g_procs[g_current].state == PROC_SLEEPING)
            g_procs[g_current].state = PROC_RUNNING;
        g_procs[g_current].ticks_rem = PROC_QUANTUM;
        return current_esp;
    }

    /* Switch */
    if (g_procs[g_current].state == PROC_RUNNING)
        g_procs[g_current].state = PROC_RUNNABLE;
    g_procs[next].state       = PROC_RUNNING;
    g_procs[next].ticks_rem   = PROC_QUANTUM;
    g_current = next;

    return g_procs[g_current].saved_esp;
}

/* ---- proc_create --------------------------------------------------------- */
/*
 * Allocates a kernel stack and builds a fake interrupt frame at its top so
 * that irq0_preempt's restoration path (pop ds; popa; add esp,8; iret) will
 * correctly jump to entry() with interrupts enabled.
 *
 * Frame layout at saved_esp (low addr = TOS):
 *   +0   ds        = 0x10
 *   +4   edi..eax  (8 × 0)   — from pusha
 *   +36  int_no    = 0x20
 *   +40  err_code  = 0
 *   +44  eip       = entry
 *   +48  cs        = 0x08
 *   +52  eflags    = 0x202   (IF=1, reserved bit 1 set)
 */
int proc_create(const char *name, void (*entry)(void))
{
    int slot;
    uint8_t  *stack;
    uint32_t *sp;

    /* Find a free slot */
    for (slot = 1; slot < PROC_MAX; slot++) {
        proc_state_t s = g_procs[slot].state;
        if (s == PROC_UNUSED || s == PROC_DEAD)
            break;
    }
    if (slot == PROC_MAX) return -1;

    /* Allocate and zero the kernel stack */
    stack = (uint8_t *)kmalloc(PROC_STACK_SIZE);
    if (!stack) return -1;
    p_memset(stack, 0, PROC_STACK_SIZE);

    /* Build initial interrupt frame */
    sp = (uint32_t *)(stack + PROC_STACK_SIZE);
    *--sp = 0x00000202;              /* eflags: IF=1 + reserved bit */
    *--sp = 0x00000008;              /* cs: kernel code segment */
    *--sp = (uint32_t)entry;         /* eip: task entry point */
    *--sp = 0;                       /* err_code */
    *--sp = 0x20;                    /* int_no */
    *--sp = 0;                       /* eax */
    *--sp = 0;                       /* ecx */
    *--sp = 0;                       /* edx */
    *--sp = 0;                       /* ebx */
    *--sp = 0;                       /* esp (inner, ignored by popa) */
    *--sp = 0;                       /* ebp */
    *--sp = 0;                       /* esi */
    *--sp = 0;                       /* edi */
    *--sp = 0x00000010;              /* ds: kernel data segment */

    /* Fill PCB */
    g_procs[slot].pid         = g_next_pid++;
    g_procs[slot].state       = PROC_RUNNABLE;
    g_procs[slot].ticks_rem   = PROC_QUANTUM;
    g_procs[slot].ticks_total = 0;
    g_procs[slot].stack_base  = stack;
    g_procs[slot].saved_esp   = (uint32_t)sp;
    p_strncpy(g_procs[slot].name, name, sizeof(g_procs[slot].name));

    return (int)g_procs[slot].pid;
}

/* ---- proc_exit ----------------------------------------------------------- */

void proc_exit(void)
{
    __asm__ volatile ("cli");
    g_procs[g_current].state = PROC_DEAD;
    __asm__ volatile ("sti");
    for (;;) __asm__ volatile ("hlt");
}

/* ---- proc_sleep / proc_wake ---------------------------------------------- */
/*
 * proc_sleep(): mark current process SLEEPING so the scheduler skips it
 * immediately on the next tick instead of burning its full quantum.
 * The caller should follow this with `hlt` to avoid a busy spin.
 *
 * proc_wake(pid): transition a SLEEPING process back to RUNNABLE.
 * Safe to call from IRQ context (mouse_handler etc.).
 */
void proc_sleep(void)
{
    __asm__ volatile ("cli");
    if (g_procs[g_current].state == PROC_RUNNING)
        g_procs[g_current].state = PROC_SLEEPING;
    __asm__ volatile ("sti");
}

void proc_wake(int pid)
{
    int i;
    if (pid < 0 || pid >= PROC_MAX) return;
    /* Find the slot with this pid */
    for (i = 0; i < PROC_MAX; i++) {
        if ((int)g_procs[i].pid == pid &&
            g_procs[i].state == PROC_SLEEPING) {
            g_procs[i].state = PROC_RUNNABLE;
            break;
        }
    }
}

/* ---- queries ------------------------------------------------------------- */

uint32_t proc_current_pid(void) { return g_procs[g_current].pid; }

int proc_count_active(void)
{
    int i, n = 0;
    for (i = 0; i < PROC_MAX; i++) {
        proc_state_t s = g_procs[i].state;
        if (s == PROC_RUNNABLE || s == PROC_RUNNING) n++;
    }
    return n;
}

pcb_t *proc_get_slot(int idx)
{
    if (idx < 0 || idx >= PROC_MAX) return 0;
    return &g_procs[idx];
}

int proc_kill(uint32_t pid)
{
    int i;
    if (pid == 0) return -1;   /* cannot kill the kernel thread */
    for (i = 0; i < PROC_MAX; i++) {
        if (g_procs[i].pid == pid) {
            proc_state_t s = g_procs[i].state;
            if (s == PROC_RUNNABLE || s == PROC_RUNNING) {
                g_procs[i].state = PROC_DEAD;
                return 0;
            }
            return -1;   /* already dead/unused */
        }
    }
    return -1;
}
