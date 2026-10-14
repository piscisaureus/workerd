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
// The entry layout is {void *ptr; uint64_t gen; uint64_t owner; uint64_t
// chain; uint64_t type; ...}, 64 bytes, index * 64 (see the table in gk.c).
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
// RDX carries the entry's type word back to ring 3 (see the type word in
// gk.h), which costs the instruction that otherwise zeroes it; a refused
// lookup leaves 0 there, as it does in RAX.
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
    shlq $6, %rdi
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
    movq 32(%rdi), %rdx                 /* the entry's type, for the caller to compare */
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
