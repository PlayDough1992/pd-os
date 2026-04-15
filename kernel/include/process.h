#pragma once

/* ============================================================================
 * PD-Kernel  —  Process Management  (Phase 10)
 * ============================================================================
 * Kernel-mode cooperative/preemptive multitasking.
 * Each process has its own kernel stack; context switches happen at IRQ0.
 * Ring 3 (user mode) is deferred to a later phase.
 * ============================================================================ */

#include "kernel.h"

#define PROC_MAX         16
#define PROC_STACK_SIZE  32768   /* 32 KB kernel stack per process */
#define PROC_QUANTUM     10      /* timer ticks per time slice (~100 ms) */

typedef enum {
    PROC_UNUSED   = 0,
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_SLEEPING,   /* voluntarily idle — skipped by scheduler */
    PROC_DEAD,
} proc_state_t;

typedef struct {
    uint32_t      pid;
    proc_state_t  state;
    char          name[24];
    uint32_t      saved_esp;    /* top of saved kernel interrupt frame       */
    uint8_t      *stack_base;   /* allocated stack (NULL for pid 0)          */
    uint32_t      ticks_total;  /* total timer ticks consumed                */
    int           ticks_rem;    /* ticks remaining in current quantum        */
} pcb_t;

/* ---- Lifecycle ---- */

/* Register the currently-running thread as pid 0; call before sti */
void      proc_init(void);

/* Create a new kernel-mode task; returns pid on success, -1 on failure */
int       proc_create(const char *name, void (*entry)(void));

/* Mark current process DEAD and spin (timer will switch away) */
void      proc_exit(void);

/* Voluntarily suspend current process until proc_wake(pid) is called.
 * Returns immediately if the process is already not sleeping.
 * Typically called from an idle loop; the scheduler skips SLEEPING tasks. */
void      proc_sleep(void);

/* Wake a sleeping process; safe to call from interrupt context */
void      proc_wake(int pid);

/* ---- Scheduler (called from irq0_preempt in sched_entry.asm) ---- */

/* Saves current_esp into present task, picks next, returns new ESP */
uint32_t  sched_irq(uint32_t current_esp);

/* ---- Queries ---- */

uint32_t  proc_current_pid(void);
int       proc_count_active(void);          /* RUNNABLE + RUNNING entries   */
pcb_t    *proc_get_slot(int slot_idx);      /* raw PCB pointer (0-based)    */
int       proc_kill(uint32_t pid);          /* 0 = success, -1 = not found  */
