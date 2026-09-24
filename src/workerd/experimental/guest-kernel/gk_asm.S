// Guest-side trampolines and exception handlers for gk. These run inside the
// guest in ring 0 and live in this binary's text, which gk maps into the guest.
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

// TLB flush the host runs on a vCPU between guest instructions: clearing and
// restoring CR4.PGE drops every one of the vCPU's TLB entries, global ones
// included, and its paging-structure caches (a CR3 reload would keep the
// global entries, which the shared runtime's pages have; see the global-pages
// section in gk.c). No host-side ioctl flushes while the guest's control
// registers stay the same. Reports on port 0xFA.
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

    .section .note.GNU-stack,"",@progbits
