// gk: the arm64 guest-side code. Included by gk_asm.S.
// arm64. Ring 0 is EL1 running on SP_EL0 (EL1t), so trusted code keeps its own
// stack; ring 3 is EL0. Every exception from either lands on the vector table
// below, at EL1 on SP_EL1, which points at this vCPU's control block (the vcb,
// struct gk_vcb in gk_arm64.h) in supervisor memory. The handler saves the
// interrupted state into the vcb and makes one hypercall (an HVC the VM's
// SMCCC filter forwards to the host); the host handles the exception from the
// vcb, writes what to do next into it, and resumes the vCPU, which falls into
// the dispatcher. So a syscall, a demand fault or a turn costs one exit and no
// register ioctl. Keep the offsets in sync with struct gk_vcb.
#define VCB_X 0           // x0..x30
#define VCB_SP 248        // SP_EL0 of the interrupted code
#define VCB_ELR 256
#define VCB_SPSR 264
#define VCB_ESR 272
#define VCB_FAR 280
#define VCB_ACTION 288    // GK_ACT_* bits, consumed by the dispatcher
#define VCB_TTBR 296      // the root GK_ACT_TTBR loads
#define VCB_TLBI_VA 304   // the address GK_ACT_TLBI_VA invalidates
#define VCB_CUR_TTBR 312  // TTBR0 as the dispatcher last left it
#define VCB_L_PC 320      // GK_ACT_LAUNCH: the turn's entry state
#define VCB_L_SP 328
#define VCB_L_X0 336
#define VCB_L_LR 344
#define VCB_L_SPSR 352
#define VCB_N_ELR 360     // an exception the handlers cannot take (see gk_exc_bad)
#define VCB_N_ESR 368
#define VCB_N_FAR 376
#define ACT_LAUNCH 0
#define ACT_FLUSH 1
#define ACT_TLBI_VA 2
#define ACT_TTBR 3
// Hypercall function ids (an SMCCC OEM range the VM forwards to the host).
#define HC_HI 0xc300
#define HC_EXC 0
#define HC_BAD 1

// The supervisor text: the vector table, the handlers and the dispatcher, in
// their own page-aligned section so that no other code shares their pages
// (gk maps [gk_super_start, gk_super_end) supervisor r-x).
    SECTION_GK_SUPER
    .p2align 12
    .globl SYM(gk_super_start)
SYM(gk_super_start):
    .globl SYM(gk_vectors)
SYM(gk_vectors):
    // Current EL with SP_EL0: ring 0 (EL1t).
    b gk_exc_sync          // synchronous
    .balign 0x80
    b gk_exc_bad           // IRQ
    .balign 0x80
    b gk_exc_bad           // FIQ
    .balign 0x80
    b gk_exc_bad           // SError
    .balign 0x80
    // Current EL with SP_ELx: an exception inside a handler or the dispatcher.
    b gk_exc_bad
    .balign 0x80
    b gk_exc_bad
    .balign 0x80
    b gk_exc_bad
    .balign 0x80
    b gk_exc_bad
    .balign 0x80
    // Lower EL, AArch64: ring 3 (EL0).
    b gk_exc_sync
    .balign 0x80
    b gk_exc_bad
    .balign 0x80
    b gk_exc_bad
    .balign 0x80
    b gk_exc_bad
    .balign 0x80
    // Lower EL, AArch32: never used.
    b gk_exc_bad
    .balign 0x80
    b gk_exc_bad
    .balign 0x80
    b gk_exc_bad
    .balign 0x80
    b gk_exc_bad
    .balign 0x80

// A synchronous exception from ring 0 or ring 3 (a syscall, an abort, an
// undefined or trapped instruction): save the interrupted state into the vcb
// (sp) and report it. The host resumes the vCPU after the HVC, into the
// dispatcher.
gk_exc_sync:
    stp x0, x1, [sp, #VCB_X + 16 * 0]
    stp x2, x3, [sp, #VCB_X + 16 * 1]
    stp x4, x5, [sp, #VCB_X + 16 * 2]
    stp x6, x7, [sp, #VCB_X + 16 * 3]
    stp x8, x9, [sp, #VCB_X + 16 * 4]
    stp x10, x11, [sp, #VCB_X + 16 * 5]
    stp x12, x13, [sp, #VCB_X + 16 * 6]
    stp x14, x15, [sp, #VCB_X + 16 * 7]
    stp x16, x17, [sp, #VCB_X + 16 * 8]
    stp x18, x19, [sp, #VCB_X + 16 * 9]
    stp x20, x21, [sp, #VCB_X + 16 * 10]
    stp x22, x23, [sp, #VCB_X + 16 * 11]
    stp x24, x25, [sp, #VCB_X + 16 * 12]
    stp x26, x27, [sp, #VCB_X + 16 * 13]
    stp x28, x29, [sp, #VCB_X + 16 * 14]
    mrs x0, sp_el0
    stp x30, x0, [sp, #VCB_X + 16 * 15]
    mrs x0, elr_el1
    mrs x1, spsr_el1
    stp x0, x1, [sp, #VCB_ELR]
    mrs x0, esr_el1
    mrs x1, far_el1
    stp x0, x1, [sp, #VCB_ESR]
    movz w0, #HC_EXC
    movk w0, #HC_HI, lsl #16
    hvc #0
    // Fall through: the host has written the vcb's action.

// The dispatcher. Every entry into the guest passes through here, with sp =
// the vcb: a fresh or reset vCPU starts here, and every other resumes here
// after its last hypercall. It applies the TLB maintenance and the root switch
// the host asked for, then either launches a turn (GK_ACT_LAUNCH) or resumes
// the saved state, with x0 (a syscall's result) as the host left it.
    .globl SYM(gk_dispatch)
SYM(gk_dispatch):
    ldr x0, [sp, #VCB_ACTION]
    str xzr, [sp, #VCB_ACTION]
    // A root switch drops every non-global translation, as a CR3 load does on
    // x86: every root runs under ASID 0, and TLBI ASIDE1 invalidates that
    // ASID's non-global entries and the walk caches, keeping the global
    // entries of the shared runtime (see the global-pages section in gk_pt.c).
    tbz x0, #ACT_TTBR, 1f
    ldr x1, [sp, #VCB_TTBR]
    dsb ishst
    msr ttbr0_el1, x1
    isb
    tlbi aside1, xzr
    dsb ish
    isb
1:  // Everything, global entries included (flush_run in gk_arm64.c).
    tbz x0, #ACT_FLUSH, 2f
    dsb ishst
    tlbi vmalle1
    dsb ish
    isb
2:  // One page, after a demand fault: an arm64 fault does not invalidate the
    // entry it was taken through, so a permission fault's stale entry goes
    // here, before the retry.
    tbz x0, #ACT_TLBI_VA, 3f
    ldr x1, [sp, #VCB_TLBI_VA]
    ubfx x1, x1, #12, #44
    dsb ishst
    tlbi vaae1, x1
    dsb ish
    isb
3:  mrs x1, ttbr0_el1
    str x1, [sp, #VCB_CUR_TTBR]
    tbnz x0, #ACT_LAUNCH, gk_launch
    ldp x0, x1, [sp, #VCB_ELR]
    msr elr_el1, x0
    msr spsr_el1, x1
    ldr x0, [sp, #VCB_SP]
    msr sp_el0, x0
    ldp x0, x1, [sp, #VCB_X + 16 * 0]
    ldp x2, x3, [sp, #VCB_X + 16 * 1]
    ldp x4, x5, [sp, #VCB_X + 16 * 2]
    ldp x6, x7, [sp, #VCB_X + 16 * 3]
    ldp x8, x9, [sp, #VCB_X + 16 * 4]
    ldp x10, x11, [sp, #VCB_X + 16 * 5]
    ldp x12, x13, [sp, #VCB_X + 16 * 6]
    ldp x14, x15, [sp, #VCB_X + 16 * 7]
    ldp x16, x17, [sp, #VCB_X + 16 * 8]
    ldp x18, x19, [sp, #VCB_X + 16 * 9]
    ldp x20, x21, [sp, #VCB_X + 16 * 10]
    ldp x22, x23, [sp, #VCB_X + 16 * 11]
    ldp x24, x25, [sp, #VCB_X + 16 * 12]
    ldp x26, x27, [sp, #VCB_X + 16 * 13]
    ldp x28, x29, [sp, #VCB_X + 16 * 14]
    ldr x30, [sp, #VCB_X + 16 * 15]
    eret

// Start a turn: fn(arg) at the privilege of the launch SPSR (EL1t for ring 0,
// EL0t for ring 3) on the launch stack, returning into gk_user_exit_tramp
// (the launch LR). No register carries anything else into the turn.
gk_launch:
    ldr x0, [sp, #VCB_L_PC]
    msr elr_el1, x0
    ldr x0, [sp, #VCB_L_SPSR]
    msr spsr_el1, x0
    ldr x0, [sp, #VCB_L_SP]
    msr sp_el0, x0
    ldr x30, [sp, #VCB_L_LR]
    ldr x0, [sp, #VCB_L_X0]
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x5, xzr
    mov x6, xzr
    mov x7, xzr
    mov x8, xzr
    mov x9, xzr
    mov x10, xzr
    mov x11, xzr
    mov x12, xzr
    mov x13, xzr
    mov x14, xzr
    mov x15, xzr
    mov x16, xzr
    mov x17, xzr
    mov x18, xzr
    mov x19, xzr
    mov x20, xzr
    mov x21, xzr
    mov x22, xzr
    mov x23, xzr
    mov x24, xzr
    mov x25, xzr
    mov x26, xzr
    mov x27, xzr
    mov x28, xzr
    mov x29, xzr
    eret

// An exception gk does not take: an interrupt, or a fault inside a handler or
// the dispatcher. Its state goes into the vcb's N fields and the host ends the
// turn and resets the vCPU to the dispatcher; it never resumes here.
gk_exc_bad:
    mrs x1, elr_el1
    str x1, [sp, #VCB_N_ELR]
    mrs x1, esr_el1
    str x1, [sp, #VCB_N_ESR]
    mrs x1, far_el1
    str x1, [sp, #VCB_N_FAR]
    movz w0, #HC_BAD
    movk w0, #HC_HI, lsl #16
    hvc #0
    b .
    .p2align 12
    .globl SYM(gk_super_end)
SYM(gk_super_end):

// The turn exit, for both rings: fn returns here with its result in x0 and
// issues the reserved syscall GK_EXIT_SYSCALL (0xF4240, see gk_internal.h), which the
// host's syscall path recognizes as the end of the turn. It runs at ring 3 for
// a ring-3 turn, so it lives on a user-executable page of its own.
    SECTION_GK_USER_EXIT
    .p2align 12
    .globl SYM(gk_user_exit_tramp)
    FUNC_TYPE(SYM(gk_user_exit_tramp))
SYM(gk_user_exit_tramp):
    movz x8, #0x4240
    movk x8, #0xf, lsl #16
    svc #0
    brk #0
    .p2align 12

    .text
#if !defined(__APPLE__)
// Host-side helpers for guest thread creation, as on x86-64 above. These run
// on the host, never in the guest. Linux only: gk does not create guest
// threads on macOS.
//
// long gk_host_clone_raw(long nr, long a1, long a2, long a3, long a4, long a5)
// The child's initial stack holds [arg, fn] at its top two words.
    .globl gk_host_clone_raw
    .type gk_host_clone_raw, @function
gk_host_clone_raw:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    svc #0
    cbz x0, 1f
    ret
1:  mov x29, xzr
    mov x30, xzr
    ldp x0, x1, [sp]   /* arg, fn */
    blr x1
    mov x8, #93        /* exit (thread) */
    svc #0
    brk #0

// void gk_host_unmapself_exit(void *addr, size_t len, long code)
    .globl gk_host_unmapself_exit
    .type gk_host_unmapself_exit, @function
gk_host_unmapself_exit:
    mov x19, x2
    mov x8, #215       /* munmap */
    svc #0
    mov x0, x19
    mov x8, #93        /* exit (thread) */
    svc #0
    brk #0
#endif

// long gk_host_call_on_stack(void *stack_top,
//                            long (*f)(void *ctx, unsigned long caller_sp),
//                            void *ctx)
// As on x86-64: caller_sp is the frame record pushed here, the lowest address
// the original stack is in use down to; x29 holds it across the call.
    .globl SYM(gk_host_call_on_stack)
    FUNC_TYPE(SYM(gk_host_call_on_stack))
SYM(gk_host_call_on_stack):
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov sp, x0         /* now on the side stack */
    mov x3, x1
    mov x0, x2         /* ctx */
    mov x1, x29        /* caller_sp */
    blr x3
    mov sp, x29        /* back to the original stack */
    ldp x29, x30, [sp], #16
    ret
