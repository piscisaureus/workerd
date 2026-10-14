// Guest-side trampolines and exception handlers for gk. These run inside the
// guest in ring 0 and live in this binary's text, which gk maps into the guest.
// The x86-64 code comes first; the arm64 code follows in its own #elif block.
//
// The arm64 code assembles for ELF (Linux) and Mach-O (macOS). Mach-O prefixes
// C-visible symbols with an underscore, names a section by segment and section,
// and has no .type directive; the macros below spell each form.
#if defined(__APPLE__)
#define SYM(name) _##name
#define FUNC_TYPE(name)
#define SECTION_GK_SUPER .section __TEXT,__gk_super,regular,pure_instructions
#define SECTION_GK_USER_EXIT .section __TEXT,__gk_user_exit,regular,pure_instructions
#else
#define SYM(name) name
#define FUNC_TYPE(name) .type name, @function
#define SECTION_GK_SUPER .section .text.gk_super,"ax",@progbits
#define SECTION_GK_USER_EXIT .section .text.gk_user_exit,"ax",@progbits
#endif
#if defined(__x86_64__)
    .text

    .globl gk_syscall_tramp
    .type gk_syscall_tramp, @function
gk_syscall_tramp:
    outb %al, $0x11
// The instruction after the hypercall. The host resumes the guest here when it
// ran a stub (below) in the middle of a syscall exit (see run_stub in gk.c), as
// KVM only completes the pending outb, i.e. skips it, when RIP is unchanged.
// This resume is for a ring-0 guest: it jumps straight back to the guest's
// saved return address. A ring-3 guest resumes at gk_user_syscall_resume
// instead (the host points RIP there), which SYSRETs back to ring 3.
    .globl gk_syscall_tramp_resume
gk_syscall_tramp_resume:
    jmpq *%rcx

// Ring-3 syscall resume: the syscall trampoline always runs in ring 0 (SYSCALL
// loads a ring-0 CS), so returning to a ring-3 guest needs SYSRET, not a plain
// jump. The host forwards the syscall (writing only RAX, preserving the RCX
// return address and R11 flags SYSCALL saved) and points RIP here; SYSRETQ then
// restores RIP<-RCX, RFLAGS<-R11 and a ring-3 CS/SS.
    .globl gk_user_syscall_resume
    .type gk_user_syscall_resume, @function
gk_user_syscall_resume:
    sysretq

// Drop into ring 3 to launch a guest function. The host sets RCX = the entry
// point, R11 = the initial RFLAGS (0x2), RSP = the user stack and RDI = the
// argument, then runs this stub in ring 0; SYSRETQ enters ring 3 at RCX with a
// ring-3 CS/SS. The function returns into gk_user_exit_tramp (pushed as its
// return address), which leaves the guest.
    .globl gk_user_launch
    .type gk_user_launch, @function
gk_user_launch:
    sysretq

// The same two entries with a page-table root switch first: the host sets
// RSI = the root the turn runs under, and RAX = the function (ring 0) or
// RCX/R11 as for gk_user_launch (ring 3). The root is loaded here, by the
// guest, rather than through KVM: under nested paging a guest MOV CR3 is not
// intercepted, and it keeps the TLB's global entries (the shared runtime's
// translations, see the global-pages section in gk.c), whereas a CR3 set
// through KVM_SET_SREGS or kvm_run makes KVM flush the whole guest TLB. The
// load drops every non-global entry, so the previous root's arena translations
// are gone before the first instruction of the turn. This page and the stack
// are in tables every root shares, so the fetch after the load walks fine. RSI
// is cleared so the root's address does not reach the turn's code.
    .globl gk_launch_cr3
    .type gk_launch_cr3, @function
gk_launch_cr3:
    movq %rsi, %cr3
    xorl %esi, %esi
    jmpq *%rax

    .globl gk_user_launch_cr3
    .type gk_user_launch_cr3, @function
gk_user_launch_cr3:
    movq %rsi, %cr3
    xorl %esi, %esi
    sysretq

// Drop into ring 3 to start a guest-created thread whose creator ran at ring 3
// (see child_entry in gk.c). The host programs the vCPU with the child's start
// registers -- its parent's at the clone syscall, with RAX = 0 and RSP = the
// stack the clone named -- and RIP = this stub, RCX = the child's start RIP
// (the guest's return address from the clone) and R11 = its RFLAGS, both as
// SYSCALL saved them in the parent. SYSRETQ enters ring 3 there with a ring-3
// CS/SS, so the child holds exactly the privilege its creator had; the kernel
// returns a native clone's child the same way. A ring-0 creator's child starts
// at its RIP directly and never runs this.
    .globl gk_user_child_launch
    .type gk_user_child_launch, @function
gk_user_child_launch:
    sysretq

// Ring-0 guest exit: report RAX on port 0xFF and halt; the host's PORT_EXIT
// handler returns it as gk_run's result. Ring-3 code cannot execute OUT, so the
// ring-3 path uses gk_user_exit_tramp instead.
    .globl gk_exit_tramp
    .type gk_exit_tramp, @function
gk_exit_tramp:
    outb %al, $0xFF
    hlt

// Ring-3 guest exit: a ring-3 function returns here with its result in RAX. OUT
// would #GP in ring 3, so the exit is a reserved sentinel syscall instead: move
// the result into RDI and issue syscall number GK_EXIT_SYSCALL (0xF4240). The
// host's PORT_SYSCALL handler recognizes the sentinel number and returns RDI as
// gk_run_user's result. Keep 0xF4240 in sync with GK_EXIT_SYSCALL in gk.c.
//
// Unlike every other stub here, this one runs in ring 3 (the guest function
// returns into it), so it must live on a user-executable page, not on the
// supervisor handler-text page. Its own page-aligned section keeps it off that
// page; gk leaves it out of the supervisor range and lets it demand-page in as
// ordinary user r-x text.
    .section .text.gk_user_exit,"ax",@progbits
    .p2align 12
    .globl gk_user_exit_tramp
    .type gk_user_exit_tramp, @function
gk_user_exit_tramp:
    movq %rax, %rdi
    movl $0xF4240, %eax
    syscall
    hlt
    .p2align 12
    .text

// Exception handlers: report the vector on port 0xFE and halt. gk_run reads the
// reported vector and CR2. Each is a distinct symbol so gk can install them.
    .globl gk_exc_de
gk_exc_de:
    movb $0x00, %al
    outb %al, $0xFE
    hlt
    .globl gk_exc_ud
gk_exc_ud:
    movq (%rsp), %rax
    outb %al, $0xFD
    hlt
    .globl gk_exc_df
gk_exc_df:
    movb $0x08, %al
    outb %al, $0xFE
    hlt
    .globl gk_exc_gp
gk_exc_gp:
    movq 8(%rsp), %rax   /* faulting RIP (error code at [rsp]) */
    outb %al, $0xFE
    hlt
    .globl gk_exc_pf
gk_exc_pf:
    outb %al, $0xFC    /* demand-page CR2: host maps it and resumes us */
    addq $8, %rsp      /* discard the #PF error code the CPU pushed */
    iretq              /* retry the faulting instruction */

// PKRU accessor the host runs on a vCPU between guest instructions: computes
// pkru = (pkru & %esi) | %edi, writes it back, and reports the new value in
// %eax on port 0xFB. The guest PKRU is only reachable through the guest's own
// rdpkru/wrpkru, so the host reads and updates it by running this stub with
// the vCPU's registers saved around it (see guest_pkru_update). Touches no
// memory, so it can run with any rsp.
    .globl gk_pkru_stub
    .type gk_pkru_stub, @function
gk_pkru_stub:
    xorl %ecx, %ecx
    rdpkru
    andl %esi, %eax
    orl %edi, %eax
    xorl %ecx, %ecx
    xorl %edx, %edx
    wrpkru
    outb %al, $0xFB
    hlt

// TLB flush the host runs on a vCPU between guest instructions. Two stubs do
// the same thing, dropping every one of the vCPU's TLB entries, global ones
// included, and its paging-structure caches (a CR3 reload would keep the
// global entries, which the shared runtime's pages have; see the global-pages
// section in gk.c); gk_init picks one for the CPU (see flush_stub_select in
// gk.c). No host-side ioctl flushes while the guest's control registers stay
// the same. Both report on port 0xFA.
//
// gk_flush_stub_invpcid: INVPCID type 2 (all contexts, global translations
// included). The instruction is legal with CR4.PCIDE clear and, for a guest
// whose CPUID carries INVPCID, not intercepted under nested paging, so the
// stub costs the one VM exit of its report. The type-2 descriptor's fields are
// ignored, but the 16-byte operand must be readable: it is the zeroed constant
// below, on this same supervisor page (which map_handler_text in gk.c maps by
// symbol, gk_invpcid_desc among them).
    .globl gk_flush_stub_invpcid
    .type gk_flush_stub_invpcid, @function
gk_flush_stub_invpcid:
    movl $2, %eax
    invpcid gk_invpcid_desc(%rip), %rax
    outb %al, $0xFA
    hlt
    .balign 16
    .globl gk_invpcid_desc
    .hidden gk_invpcid_desc   /* RIP-relative above: must resolve within this object */
gk_invpcid_desc:
    .quad 0, 0

// gk_flush_stub: clearing and restoring CR4.PGE, for a CPU without INVPCID.
// Each CR4 write is intercepted, so this stub costs three VM exits.
    .globl gk_flush_stub
    .type gk_flush_stub, @function
gk_flush_stub:
    movq %cr4, %rax
    movq %rax, %rcx
    andq $~0x80, %rcx      /* CR4.PGE */
    movq %rcx, %cr4
    movq %rax, %cr4
    outb %al, $0xFA
    hlt

// Host-side helpers for guest thread creation. These run on the host, never in
// the guest.
//
// long gk_host_clone_raw(long nr, long a1, long a2, long a3, long a4, long a5)
// Issues clone (56) or clone3 (435) with the given arguments and returns the
// result to the parent. The child's initial stack must hold [arg, fn] at its
// top two words: the child pops them, calls fn(arg) on that stack, and exits the
// thread with fn's return value. A C caller cannot survive a raw clone (the
// child would return into a frame that lives on the parent's stack), hence asm.
    .globl gk_host_clone_raw
    .type gk_host_clone_raw, @function
gk_host_clone_raw:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    movq %r8, %r10
    movq %r9, %r8
    syscall
    testq %rax, %rax
    jz 1f
    ret
1:  xorl %ebp, %ebp
    popq %rdi          /* arg */
    popq %rax          /* fn */
    call *%rax
    movq %rax, %rdi
    movl $60, %eax     /* exit (thread) */
    syscall
    hlt

// void gk_host_unmapself_exit(void *addr, size_t len, long code)
// Unmaps the stack this thread is running on, then exits the thread with
// `code`. Nothing touches the stack between the two syscalls.
    .globl gk_host_unmapself_exit
    .type gk_host_unmapself_exit, @function
gk_host_unmapself_exit:
    movq %rdx, %r12
    movl $11, %eax     /* munmap */
    syscall
    movq %r12, %rdi
    movl $60, %eax     /* exit (thread) */
    syscall
    hlt

// long gk_host_call_on_stack(void *stack_top,
//                            long (*f)(void *ctx, unsigned long caller_sp),
//                            void *ctx)
// Calls f(ctx, caller_sp) with rsp switched to `stack_top` (16-byte aligned),
// then switches back and returns f's result. `caller_sp` is the lowest address
// this thread's original stack is in use down to at the moment of the switch:
// everything below it is free for the guest (see gk_run_here). The frame
// pointer holds the original rsp across the call, as f must preserve it.
    .globl gk_host_call_on_stack
    .type gk_host_call_on_stack, @function
gk_host_call_on_stack:
    pushq %rbp
    movq %rsp, %rbp    /* original stack: [saved rbp][return address]... */
    movq %rdi, %rsp    /* now on the side stack; ABI-aligned for the call below */
    movq %rsi, %rax
    movq %rdx, %rdi    /* ctx */
    movq %rbp, %rsi    /* caller_sp */
    call *%rax
    movq %rbp, %rsp    /* back to the original stack */
    popq %rbp
    ret

#elif defined(__aarch64__)
// arm64. Ring 0 is EL1 running on SP_EL0 (EL1t), so trusted code keeps its own
// stack; ring 3 is EL0. Every exception from either lands on the vector table
// below, at EL1 on SP_EL1, which points at this vCPU's control block (the vcb,
// struct gk_vcb in gk.c) in supervisor memory. The handler saves the
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
    // entries of the shared runtime (see the global-pages section in gk.c).
    tbz x0, #ACT_TTBR, 1f
    ldr x1, [sp, #VCB_TTBR]
    dsb ishst
    msr ttbr0_el1, x1
    isb
    tlbi aside1, xzr
    dsb ish
    isb
1:  // Everything, global entries included (flush_run in gk.c).
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
// issues the reserved syscall GK_EXIT_SYSCALL (0xF4240, see gk.c), which the
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

#else
#error "gk supports x86-64 and arm64 only"
#endif

#if !defined(__APPLE__)
    .section .note.GNU-stack,"",@progbits
#endif
