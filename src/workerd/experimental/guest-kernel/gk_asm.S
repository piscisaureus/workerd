// Guest-side trampolines and exception handlers for gk. These run inside the
// guest in ring 0 and live in this binary's text, which gk maps into the guest.
    .text

// SYSCALL entry (MSR_LSTAR). A ring-0 resolve (see gk_resolve in gk.h) is
// answered here, inside the guest, by gk_resolve_tramp below; everything else
// is the outb hypercall that leaves the VM for the host's syscall exit. The
// compare costs the forwarded path one predictable branch. Keep 0xF4242 in
// sync with GK_RESOLVE_SYSCALL in gk.c.
    .globl gk_syscall_tramp
    .type gk_syscall_tramp, @function
gk_syscall_tramp:
    cmpq $0xF4242, %rax
    je gk_resolve_tramp
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

// The ring-0 resolve (see gk_resolve in gk.h): RDI holds the handle, RCX and
// R11 the ring-3 return RIP and RFLAGS that SYSCALL saved. Looks the handle up
// in gk_resolve_table (gk.c; supervisor memory, mapped in every root) and
// SYSRETs to ring 3 with the entry's pointer in RAX, or 0 for a handle that
// names nothing: a zero generation (never issued), an entry whose generation
// is not the handle's, an entry reserved and not yet committed, or an entry
// owned by a page-table root other than the one this caller runs under. The
// index is the handle's low 20 bits and the table holds exactly 2^20 entries,
// so the mask bounds it. Runs on whatever RSP ring 3 had and never touches
// it, so a ring-3 stack that is
// unmapped or garbage costs nothing here; reads only the table, so no value
// in the handle is ever used as an address; writes nothing, so the table is
// as the host left it and the path needs no lock. The generation is read
// before and after the pointer and both reads must match the handle: the
// host retires an entry by zeroing the generation first and publishes one by
// storing the pointer first (gk.c), and x86 keeps loads in program order, so
// a pointer seen between two matching reads was the entry's under that
// generation. Every scratch register is cleared before SYSRET, so ring 3 gets
// the pointer and nothing else; RCX and R11 are untouched for SYSRET to use.
// The entry layout is {void *ptr; uint64_t gen; uint64_t owner; uint64_t},
// 32 bytes, index * 32.
// Keep the split at bit 20 in sync with GK_RESOLVE_INDEX_BITS in gk.h.
//
// The owner is the arena root the host registered the entry under, and CR3 is
// the root the caller is running under: the one piece of caller identity that
// is not memory, since ring 3 can neither read nor load CR3 and the host
// alone decides which root a turn enters with. Comparing the two is what
// makes a handle one isolate's: a turn that rewrites its own handle word to
// another isolate's live handle gets 0, exactly as it does for a stale one.
// A reserved entry is refused by its missing pointer before the owner is
// looked at, so that it resolves to nothing whatever the tag holds; the host
// commits the owner before the pointer and the pointer is read first, so an
// owner read behind a committed pointer is the one the entry was committed
// with. The owner is read between the two generation reads, as the pointer
// is: the generations entries are registered under run consecutively, so a
// caller can hold the next one before it is issued, and an owner read
// outside that window could be the next registration's rather than this
// one's. CR3 is read into RSI, which is already a scratch register the exit
// clears, so the root never reaches ring 3 and no further register is used.
//
// GK_NEG_RESOLVE (a negative-test build, never shipped) removes the
// generation check, so a handle to a freed or recycled entry resolves to
// whatever the slot holds; GK_NEG_RESOLVE_OWNER removes the owner check, so
// another arena's live handle resolves to its object. gk_test's resolve case
// must fail under either.
    .globl gk_resolve_tramp
    .type gk_resolve_tramp, @function
gk_resolve_tramp:
    movq %rdi, %rax
    shrq $20, %rax                      /* rax = the handle's generation */
    jz 1f
    andl $0xfffff, %edi                 /* edi = the handle's index, below the table's capacity */
    shlq $5, %rdi
    leaq gk_resolve_table(%rip), %rsi
    addq %rsi, %rdi                     /* rdi = &table[index] */
    movq 8(%rdi), %rsi                  /* generation, before */
    movq (%rdi), %rdx                   /* pointer */
#ifndef GK_NEG_RESOLVE
    cmpq %rsi, %rax
    jne 1f
#endif
    testq %rdx, %rdx                    /* a reserved entry has no pointer */
    jz 1f
    movq %cr3, %rsi                     /* the root this caller runs under */
#ifndef GK_NEG_RESOLVE_OWNER
    cmpq 16(%rdi), %rsi                 /* the root the entry was registered under */
    jne 1f
#endif
#ifndef GK_NEG_RESOLVE
    cmpq 8(%rdi), %rax                  /* generation, after */
    jne 1f
#endif
    movq %rdx, %rax
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    sysretq
1:  xorl %eax, %eax
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    sysretq
    .hidden gk_resolve_table            /* RIP-relative above: must resolve within this object */

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

// ---- resident vCPUs: the park loop ------------------------------------------
// A resident vCPU (GK_RUNNER=resident; see the runner section in gk.c) never
// leaves KVM_RUN to end or start a turn. Between turns it runs this loop at
// ring 0, with GS pointing at its park frame (gk_park in gk.c: supervisor
// memory, host-written), which carries the next turn's entry state in and
// the finished turn's result out. The frame's offsets are fixed here and
// checked against the struct by gk.c.
//
// The loop runs on no stack of the turn's: the host sets RSP to the frame's
// ring-0 stack, and nothing here pushes. An NMI (the host's kick, below)
// takes the IST2 stack and returns with IRETQ, so it is safe at any point:
// here, in a stub, or in the middle of a ring-3 turn.
#define PARK_GO 0
#define PARK_DONE 8
#define PARK_RESULT 16
#define PARK_CR3 24
#define PARK_RSP 32
#define PARK_FN 40
#define PARK_ARG 48
#define PARK_USER 56
#define PARK_FLUSH 64
#define PARK_SPIN 72
#define PARK_HALTED 80
#define PARK_OWNER_SLEEPING 88
#define PARK_LEAVE 96
#define PARK_PORT 104
#define PARK_USER_GS 112
#define PARK_STACK 120
#define PARK_RELOAD 128
#define PARK_CUR_CR3 136
#define PARK_IN_TURN 144

// Entry from the host: RDI = the frame. Sets GS base to the frame and
// KERNEL_GS_BASE to the turn's GS base unconditionally, whatever state a
// previous exit left the pair in (a fault mid-turn leaves them swapped one
// way, a fault in the loop the other), then parks. Both MSRs are direct
// access under KVM, so this costs no exit.
    .globl gk_park_entry
    .type gk_park_entry, @function
gk_park_entry:
    movq %rdi, %rax
    movq %rdi, %rdx
    shrq $32, %rdx
    movl $0xC0000101, %ecx          /* MSR_GS_BASE */
    wrmsr
    movq PARK_USER_GS(%rdi), %rax
    movq %rax, %rdx
    shrq $32, %rdx
    movl $0xC0000102, %ecx          /* MSR_KERNEL_GS_BASE */
    wrmsr
    jmp gk_park_loop

// Ring-0 guest exit for a resident vCPU: the turn's return address. RAX is
// the result.
    .globl gk_park_exit_tramp
    .type gk_park_exit_tramp, @function
gk_park_exit_tramp:
    movq %rax, %rdi
    jmp gk_park_exit

// SYSCALL entry (MSR_LSTAR) for a resident vCPU: the resolve is answered as
// in gk_syscall_tramp, the exit sentinel (GK_EXIT_SYSCALL, 0xF4240) parks the
// vCPU with RDI as the turn's result instead of leaving the VM, and every
// other syscall is the hypercall as before. The host resumes a ring-0 guest
// after a stub run at gk_syscall_tramp_resume, which is the same jmpq *%rcx.
    .globl gk_park_syscall_tramp
    .type gk_park_syscall_tramp, @function
gk_park_syscall_tramp:
    cmpq $0xF4242, %rax
    je gk_resolve_tramp
    cmpq $0xF4240, %rax
    je gk_park_exit
    outb %al, $0x11
    jmpq *%rcx

// The turn is over: RDI = its result, GS still the turn's. Publish the
// result and echo the turn's sequence word into `done` with a locked
// exchange, so the owner that sees `done` sees the result, and so this
// store is ordered before the load of `owner_sleeping` that follows (x86
// reorders a plain store past a later load). An owner that said it sleeps
// is woken through the ioeventfd on the frame's port, which KVM services in
// the kernel; otherwise it is spinning on `done` and nothing more is needed.
    .globl gk_park_exit
    .type gk_park_exit, @function
gk_park_exit:
    swapgs
    movq %rdi, %gs:PARK_RESULT
    movq $0, %gs:PARK_IN_TURN
    movq %gs:PARK_GO, %rax
    xchgq %rax, %gs:PARK_DONE
    cmpq $0, %gs:PARK_OWNER_SLEEPING
    je gk_park_loop
    movq %gs:PARK_PORT, %rdx
    xorl %eax, %eax
    outb %al, %dx
    /* falls through */

// Wait for the next turn. `go` moves ahead of `done` when the host has
// written the frame; the loop spins the frame's budget (negative: for ever)
// and then halts: it raises `halted` with a locked exchange, re-reads `go`
// and `leave` (an owner that bumped either before this vCPU raised the flag
// sends no kick, and is seen here instead; one that bumped it after sees the
// flag and kicks), and executes STI;HLT. An NMI wakes HLT whatever IF is,
// and it is not held back by the STI shadow as a maskable interrupt would
// be, so a kick can land anywhere between the raise of `halted` and the
// HLT, where a plain return would leave the vCPU halting on a turn already
// posted: the NMI handler (gk_exc_nmi) returns past the HLT instead when it
// finds the interrupted RIP in that window, and the loop re-checks. `leave`
// asks the loop to exit to userspace (port 0xF8), for the host operations
// that need the vCPU out of KVM_RUN. `reload` names a root the loop is to
// load at once and then clear, for a host that is about to free the root
// the vCPU parked under (see park_reload_root in gk.c); a launch clears it
// too, since the launch loads the root the turn wants. `cur_cr3` is the
// root this loop is under, written here at every load, which is how the
// host tells which parked vCPUs a root it is about to free still carries.
// `in_turn` is set at the launch and cleared before `done` is echoed, so
// the host can tell a turn reported ended from one still running.
    .globl gk_park_loop
    .type gk_park_loop, @function
gk_park_loop:
    movq %gs:PARK_STACK, %rsp
    movq %gs:PARK_SPIN, %rcx
1:  movq %gs:PARK_RELOAD, %rax
    testq %rax, %rax
    jz 0f
    movq %rax, %cr3
    movq %rax, %gs:PARK_CUR_CR3
    movq $0, %gs:PARK_RELOAD
0:  cmpq $0, %gs:PARK_LEAVE
    jne 9f
    movq %gs:PARK_GO, %rax
    cmpq %gs:PARK_DONE, %rax
    jne 5f
    testq %rcx, %rcx
    jz 2f
    js 3f
    decq %rcx
3:  pause
    jmp 1b
2:  movl $1, %eax
    xchgq %rax, %gs:PARK_HALTED
gk_park_hlt_window:
    cmpq $0, %gs:PARK_LEAVE
    jne 4f
    cmpq $0, %gs:PARK_RELOAD
    jne 4f
    movq %gs:PARK_GO, %rax
    cmpq %gs:PARK_DONE, %rax
    jne 4f
    sti
gk_park_hlt:
    hlt
gk_park_hlt_resume:
    cli
4:  movq $0, %gs:PARK_HALTED
    movq %gs:PARK_SPIN, %rcx
    jmp 1b
    // Launch. A pending full TLB flush first (1: INVPCID all contexts, 2:
    // the CR4.PGE toggle, as flush_stub_select chose; see the flush stubs
    // above), then the root the turn runs under, if the host asked for a
    // load (see gk_launch_cr3), then the entry registers; every other
    // register is cleared so nothing of this loop reaches the turn, and GS
    // is swapped back to the turn's.
5:  movq %gs:PARK_FLUSH, %rax
    testq %rax, %rax
    jz 6f
    cmpq $1, %rax
    jne 7f
    movl $2, %eax
    invpcid gk_invpcid_desc(%rip), %rax
    jmp 8f
7:  movq %cr4, %rax
    movq %rax, %rdx
    andq $~0x80, %rdx
    movq %rdx, %cr4
    movq %rax, %cr4
8:  movq $0, %gs:PARK_FLUSH
6:  movq $0, %gs:PARK_RELOAD
    movq %gs:PARK_CR3, %rsi
    testq %rsi, %rsi
    jz 1f
    movq %rsi, %cr3
    movq %rsi, %gs:PARK_CUR_CR3
1:  movq $1, %gs:PARK_IN_TURN
    movq %gs:PARK_RSP, %rsp
    movq %gs:PARK_ARG, %rdi
    movq %gs:PARK_FN, %rcx
    movq %gs:PARK_USER, %rax
    movl $2, %r11d
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %ebx, %ebx
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    testq %rax, %rax
    jz 2f
    xorl %eax, %eax
    swapgs
    sysretq
2:  movq %rcx, %rax
    xorl %ecx, %ecx
    xorl %r11d, %r11d
    swapgs
    jmpq *%rax
9:  outb %al, $0xF8
    jmp gk_park_loop

// The NMI the host kicks a halted resident vCPU with. Delivered on IST2, so
// it never shares a stack with the exception handlers below. It returns
// where it landed, HLT having been woken, except when it landed in the park
// loop between the raise of `halted` and the HLT (see gk_park_loop): a
// return there would run the HLT with the kick already consumed, so the
// saved RIP is moved past it. Inclusive of the HLT's own address, which is
// where an NMI taken before the HLT executes reports; an NMI that woke the
// HLT reports the instruction after it. Touches nothing but RAX, restored.
    .globl gk_exc_nmi
    .type gk_exc_nmi, @function
gk_exc_nmi:
    pushq %rax
    pushq %rcx
    movq 16(%rsp), %rax                 /* the interrupted RIP */
    leaq gk_park_hlt_window(%rip), %rcx
    subq %rcx, %rax                     /* offset into the window, unsigned */
    cmpq $(gk_park_hlt - gk_park_hlt_window), %rax
    ja 1f
    leaq gk_park_hlt_resume(%rip), %rcx
    movq %rcx, 16(%rsp)
1:  popq %rcx
    popq %rax
    iretq

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

    .section .note.GNU-stack,"",@progbits
