// gk: x86-64 paging, control-register and hypercall definitions. Included by gk_internal.h.
#ifndef GK_X86_64_H
#define GK_X86_64_H

// ---- x86-64 paging and control-register bits -------------------------------
#define PTE_P (1UL << 0)
#define PTE_W (1UL << 1)
#define PTE_U (1UL << 2)
#define PTE_G (1UL << 8)         // global: the translation survives a CR3 load (see the global-pages section)
#define PTE_WALL (1UL << 9)      // CPU-ignored software bit: W withheld by the host-frame wall
#define PTE_NX (1UL << 63)
#define PTE_PKEY_SHIFT 59        // bits 62:59 hold the page's protection key
#define PTE_PKEY_MASK (0xfUL << PTE_PKEY_SHIFT)
#define PF_ERR_WR (1UL << 1)     // #PF error code: the access was a write
#define PF_ERR_PK (1UL << 5)     // #PF error code: protection-key violation
#define CR0_PE (1UL << 0)
#define CR0_WP (1UL << 16)
#define CR0_PG (1UL << 31)
#define CR4_PAE (1UL << 5)
#define CR4_PGE (1UL << 7)
#define CR4_OSFXSR (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)
#define CR4_OSXSAVE (1UL << 18)
#define CR4_PKE (1UL << 22)
#define EFER_LME (1UL << 8)
#define EFER_LMA (1UL << 10)
#define EFER_SCE (1UL << 0)
#define EFER_NXE (1UL << 11)

#define MSR_EFER 0xc0000080
#define MSR_STAR 0xc0000081
#define MSR_LSTAR 0xc0000082
#define MSR_SYSCALL_MASK 0xc0000084

// Hypercall ports.
#define PORT_SYSCALL 0x11
#define PORT_EXIT 0xFF
#define PORT_FAULT 0xFE
#define PORT_UDRIP 0xFD
#define PORT_DEMAND 0xFC
#define PORT_PKRU 0xFB
#define PORT_FLUSH 0xFA
#define PORT_PARK_LEAVE 0xF8  // a resident vCPU's park loop leaving KVM_RUN on request

// The PTE operations the shared page-table code is written in (see the arm64
// definitions in gk_arm64.h for why they are not plain bit operations).
#define PTE_TABLE_ENTRY(p) (((uint64_t)(uintptr_t)(p)) | PTE_P | PTE_W | PTE_U)
#define PTE_TABLE_PTR(e) ((uint64_t *)(uintptr_t)((e) & ~0xfffULL))
#define PTE_IS_W(e) ((e) & PTE_W)
#define PTE_SET_W(e) ((e) | PTE_W)
#define PTE_CLR_W(e) ((e) & ~PTE_W)
#define GK_HAS_PKEYS 1           // the guest's protection keys are virtualized (see that section)
#define GK_USER_SLOTS 256        // PML4 slots 0..255 are the 47-bit user address space

typedef struct kvm_regs gk_regs;

#endif  // GK_X86_64_H
