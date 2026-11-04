/*
 * start.S - AVR32 reset entry, event table and trampolines for the self-test and
 * the fuzz target, for both microarchitectures (AVR32B builds define AVR32B).
 *
 * The event table (EVBA = _evba) sends every exception to exc_common, which saves
 * r0-r12 and lr above SR and the return pc (AVR32A: the SR and pc the processor
 * pushed on the system stack; AVR32B: copied from RSR_EX / RAR_EX), calls
 * trap_handler(frame), and returns through rete with the frame as the handler left
 * it. The breakpoint entry (debug mode) counts in g_bkpt and resumes after the
 * breakpoint instruction; the scall entry returns r12 * 2 + 1 through rets. The
 * autovectored interrupt stubs call irq_handler(level).
 */
#define SYS_SR      0
#define SYS_EVBA    4
#define SYS_RSR_SUP 20
#define SYS_RSR_EX  40
#define SYS_RAR_SUP 52
#define SYS_RAR_EX  72
#define SYS_RAR_DBG 80

    .section .text.init, "ax"
    .globl _start
_start:
    lddpc   sp, .Lstack_top
    lddpc   r0, .Levba
    mtsr    SYS_EVBA, r0
    csrf    21                  /* EM: exceptions are now recoverable */
    lddpc   r0, .Lbss_start
    lddpc   r1, .Lbss_end
    mov     r2, 0
1:  cp.w    r0, r1
    brhs    2f
    st.w    r0++, r2
    rjmp    1b
2:  rcall   main
    lddpc   r0, .Lfinisher      /* main returns the finisher word */
    st.w    r0[0], r12
3:  rjmp    3b

    .balign 4
.Lstack_top: .long __stack_top
.Levba:      .long _evba
.Lbss_start: .long __bss_start
.Lbss_end:   .long __bss_end
.Lfinisher:  .long 0xffff1800

/* ---- event table ------------------------------------------------------------ */

    .section .text.evba, "ax"
    .balign 0x200
    .globl _evba
_evba:
    .org 0x00
    rjmp    exc_common          /* unrecoverable */
    .org 0x04
    rjmp    exc_common          /* TLB multiple hit */
    .org 0x08
    rjmp    exc_common          /* bus error, data */
    .org 0x0c
    rjmp    exc_common          /* bus error, instruction fetch */
    .org 0x10
    rjmp    exc_common          /* NMI */
    .org 0x14
    rjmp    exc_common          /* instruction address */
    .org 0x18
    rjmp    exc_common          /* ITLB protection */
    .org 0x1c
    rjmp    bkpt_entry          /* breakpoint */
    .org 0x20
    rjmp    exc_common          /* illegal opcode */
    .org 0x24
    rjmp    exc_common          /* unimplemented instruction */
    .org 0x28
    rjmp    exc_common          /* privilege violation */
    .org 0x2c
    rjmp    exc_common          /* floating point */
    .org 0x30
    rjmp    exc_common          /* coprocessor absent */
    .org 0x34
    rjmp    exc_common          /* data address, read */
    .org 0x38
    rjmp    exc_common          /* data address, write */
    .org 0x3c
    rjmp    exc_common          /* DTLB protection, read */
    .org 0x40
    rjmp    exc_common          /* DTLB protection, write */
    .org 0x44
    rjmp    exc_common          /* DTLB modified */
    .org 0x50
    rjmp    exc_common          /* ITLB miss */
    .org 0x60
    rjmp    exc_common          /* DTLB miss, read */
    .org 0x70
    rjmp    exc_common          /* DTLB miss, write */

    .org 0x100                  /* scall */
    lsl     r12, 1
    sub     r12, -1
    rets

exc_common:
#ifdef AVR32B
    sub     sp, 8
#endif
    stm     --sp, r0-r12, lr
#ifdef AVR32B
    mfsr    r0, SYS_RSR_EX
    st.w    sp[56], r0
    mfsr    r0, SYS_RAR_EX
    st.w    sp[60], r0
#endif
    mov     r12, sp
    rcall   trap_handler
#ifdef AVR32B
    ld.w    r0, sp[56]
    mtsr    SYS_RSR_EX, r0
    ld.w    r0, sp[60]
    mtsr    SYS_RAR_EX, r0
#endif
    ldm     sp++, r0-r12, lr
#ifdef AVR32B
    sub     sp, -8
#endif
    rete

bkpt_entry:
    st.w    --sp, r0
    st.w    --sp, r1
    mfsr    r0, SYS_RAR_DBG
    sub     r0, -2              /* resume after the breakpoint */
    mtsr    SYS_RAR_DBG, r0
    lddpc   r1, .Lbkpt
    ld.w    r0, r1[0]
    sub     r0, -1
    st.w    r1[0], r0
    ld.w    r1, sp++
    ld.w    r0, sp++
    retd

    .balign 4
.Lbkpt: .long g_bkpt

/* Autovectored interrupt handlers: offsets from _evba go in the controller's IPRs.
 * AVR32A stacks r8-r12 and lr itself; AVR32B (but for INT3's shadow registers)
 * does not, so the stubs keep them for both. */
    .macro irq_stub level
    .balign 4
    .globl int\level\()_stub
int\level\()_stub:
    stm     --sp, r8-r12, lr
    mov     r12, \level
    rcall   irq_handler
    ldm     sp++, r8-r12, lr
    rete
    .endm
    irq_stub 0
    irq_stub 1
    irq_stub 2
    irq_stub 3

/* ---- helpers for the tests ---------------------------------------------------- */

    .text
/* probe_ld(addr) / probe_st(addr, v) / probe_call(fn): one access (or call); a
 * fault resumes at probe_ret with r12 = what trap_handler chose. Returns use
 * retal: the compiler takes the flags from r12 after a call. */
    .globl probe_ld, probe_st, probe_ldh, probe_call, probe_ret
probe_ld:
    ld.w    r12, r12[0]
    retal   r12
probe_ldh:
    ld.sh   r12, r12[0]
    retal   r12
probe_st:
    st.w    r12[0], r11
    retal   0
probe_call:
    pushm   lr
    icall   r12
    popm    lr
probe_ret:
    retal   r12

/* enter_user(pc, arg, usp): run pc in application mode with r12 = arg on the stack
 * usp; trap_handler ends the excursion at user_resume, which returns from here
 * with r12 as the frame left it. */
    .globl enter_user, user_resume
enter_user:
    pushm   r0-r7, lr
    lddpc   r9, .Lsaved
    st.w    r9[0], sp
    st.w    r9[4], r10
    sub     r9, -4
    ldmts   r9, sp              /* SP_APP = usp */
    mfsr    r8, SYS_SR
    andh    r8, 0xfe3f          /* M = application */
#ifdef AVR32B
    mtsr    SYS_RAR_SUP, r12
    mtsr    SYS_RSR_SUP, r8
#else
    st.w    --sp, r12           /* rets pops SR, then pc */
    st.w    --sp, r8
#endif
    mov     r12, r11
    rets
user_resume:
    lddpc   r9, .Lsaved
    ld.w    sp, r9[0]
    popm    r0-r7, lr
    retal   r12

    .balign 4
.Lsaved: .long g_saved

/* Application-mode code: scall (r12 = r12 * 2 + 1), then a privileged instruction. */
    .globl user_stub
user_stub:
    scall
    mov     r11, r12
    mfsr    r10, SYS_EVBA
    rjmp    user_stub

    .section .bss, "aw", @nobits
    .balign 4
    .globl g_bkpt, g_saved
g_bkpt:  .space 4
g_saved: .space 8
