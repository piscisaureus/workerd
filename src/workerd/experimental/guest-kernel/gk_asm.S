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
    .globl gk_syscall_tramp_resume
gk_syscall_tramp_resume:
    jmpq *%rcx

    .globl gk_exit_tramp
    .type gk_exit_tramp, @function
gk_exit_tramp:
    outb %al, $0xFF
    hlt

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

// TLB flush the host runs on a vCPU between guest instructions: reloading CR3
// drops the vCPU's (non-global) TLB entries, which no host-side ioctl does
// while the guest's control registers stay the same. Reports on port 0xFA.
    .globl gk_flush_stub
    .type gk_flush_stub, @function
gk_flush_stub:
    movq %cr3, %rax
    movq %rax, %cr3
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

    .section .note.GNU-stack,"",@progbits
