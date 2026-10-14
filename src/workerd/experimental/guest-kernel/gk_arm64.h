// gk: arm64 paging, exception, vcb and system-register definitions. Included by gk_internal.h.
#ifndef GK_ARM64_H
#define GK_ARM64_H

// ---- arm64 paging --------------------------------------------------------------
// 48-bit VA, 4KiB granule: four levels indexed by exactly the bits x86-64's
// PML4/PDPT/PD/PT use, so the page-table code is shared and the names (pml4,
// pdpt, ...) are the x86 ones. What differs is the entries. A table entry
// holds the guest-physical address of the next table, and guest-physical is
// not host-virtual here (see the guest-physical allocator), so the shared
// code converts through PTE_TABLE_ENTRY/PTE_TABLE_PTR. A leaf is a page
// descriptor:
#define PTE_P (1UL << 0)         // valid; with bit 1, a table or (at level 3) a page
#define PTE_TABLE (1UL << 1)
#define PTE_U (1UL << 6)         // AP[1]: EL0 (ring 3) may access the page
#define PTE_RDONLY (1UL << 7)    // AP[2]: read-only, at every EL. W is its inverse
#define PTE_SH_INNER (3UL << 8)  // inner shareable
#define PTE_AF (1UL << 10)       // access flag, set up front: the guest takes no access-flag faults
#define PTE_NG (1UL << 11)       // not global: cleared for a global page (see the global-pages section)
#define PTE_PXN (1UL << 53)      // no execution at EL1
#define PTE_UXN (1UL << 54)      // no execution at EL0
#define PTE_WALL (1UL << 55)     // software bit: W withheld by the host-frame wall
#define PTE_ADDR_MASK 0x0000fffffffff000ULL
// AttrIndx is 0: MAIR_EL1 attribute 0 is normal write-back memory (vcpu_init).
#define PTE_TABLE_ENTRY(p) (PT_GPA(p) | PTE_P | PTE_TABLE)
#define PTE_TABLE_PTR(e) ((uint64_t *)(uintptr_t)(G->pt_base + (((e) & PTE_ADDR_MASK) - G->pt_gpa)))
#define PT_GPA(p) (G->pt_gpa + ((uintptr_t)(p) - G->pt_base))
#define PTE_IS_W(e) (!((e) & PTE_RDONLY))
#define PTE_SET_W(e) ((e) & ~PTE_RDONLY)
#define PTE_CLR_W(e) ((e) | PTE_RDONLY)
#define PTE_SET_GLOBAL(e) ((e) & ~PTE_NG)
// No memory protection keys (POE) on the CPUs this targets: pkey syscalls are
// forwarded to the host unchanged and no PTE carries a key.
#define GK_HAS_PKEYS 0
#define GK_USER_SLOTS 512        // level-0 slots 0..511 are the 48-bit user address space

// The ESR_EL1 classes and fault status codes run_vcpu tells apart.
#define ESR_EC(esr) (((esr) >> 26) & 0x3f)
#define ESR_EC_WFX 0x01
#define ESR_EC_SVC64 0x15
#define ESR_EC_HVC64 0x16
#define ESR_EC_IABT_LOW 0x20
#define ESR_EC_IABT_CUR 0x21
#define ESR_EC_DABT_LOW 0x24
#define ESR_EC_DABT_CUR 0x25
#define ESR_WNR (1UL << 6)       // data abort: the access was a write
#define ESR_FNV (1UL << 10)      // data abort: FAR is not valid
// PSTATE/SPSR modes, with D, A, I and F masked: EL0t (ring 3), EL1t (ring 0,
// on SP_EL0) and EL1h (the handlers and the dispatcher, on SP_EL1 = the vcb).
#define PSR_EL0T 0x3c0
#define PSR_EL1T 0x3c4
#define PSR_EL1H 0x3c5
// Hypercall function ids the vector handlers issue (see gk_asm_arm64.S); an SMCCC
// OEM-service range the VM's SMCCC filter forwards to the host.
#define GK_HC_BASE 0xc3000000u
#define GK_HC_EXC (GK_HC_BASE + 0)
#define GK_HC_BAD (GK_HC_BASE + 1)
#define GK_HC_COUNT 16
// vcb action bits (see gk_dispatch in gk_asm_arm64.S). Without GK_ACT_LAUNCH the
// dispatcher resumes the saved state.
#define GK_ACT_LAUNCH (1UL << 0)
#define GK_ACT_FLUSH (1UL << 1)
#define GK_ACT_TLBI_VA (1UL << 2)
#define GK_ACT_TTBR (1UL << 3)
// The interrupted state a vector handler saves (gk_exc_sync in gk_asm_arm64.S), and
// the register file of a guest-created thread's start (see clone_thread).
typedef struct gk_frame {
  uint64_t x[31];
  uint64_t sp;             // SP_EL0
  uint64_t elr, spsr, esr, far;
} gk_frame;
// The vcb: a vCPU's control block, the first bytes of its supervisor region
// (gk_thread.ist) and its SP_EL1. The guest's handlers and dispatcher read and
// write it at EL1; the host reads and writes it only while the vCPU is out of
// KVM_RUN. Ring 3 cannot reach it. Offsets are fixed by gk_asm_arm64.S.
struct gk_vcb {
  gk_frame f;
  uint64_t action;         // GK_ACT_*: what the dispatcher does next
  uint64_t ttbr;           // GK_ACT_TTBR: the root to load
  uint64_t tlbi_va;        // GK_ACT_TLBI_VA: the address to invalidate
  uint64_t cur_ttbr;       // TTBR0 as the dispatcher last left it
  uint64_t l_pc, l_sp, l_x0, l_lr, l_spsr;  // GK_ACT_LAUNCH: the turn's entry
  uint64_t n_elr, n_esr, n_far;             // an exception gk does not take (gk_exc_bad)
};
_Static_assert(offsetof(struct gk_vcb, f.sp) == 248 && offsetof(struct gk_vcb, f.elr) == 256 &&
                   offsetof(struct gk_vcb, f.esr) == 272 && offsetof(struct gk_vcb, action) == 288 &&
                   offsetof(struct gk_vcb, cur_ttbr) == 312 && offsetof(struct gk_vcb, l_pc) == 320 &&
                   offsetof(struct gk_vcb, l_spsr) == 352 && offsetof(struct gk_vcb, n_far) == 376,
               "struct gk_vcb must match the VCB_* offsets in gk_asm_arm64.S");
#define VCB(t) ((struct gk_vcb *)(uintptr_t)(t)->ist)
typedef gk_frame gk_regs;
#define GK_REGS_SP(r) ((r).sp)
#if !GK_HVF
// One register of a vCPU, by KVM_{GET,SET}_ONE_REG id: GK_CORE_REG names a
// field of struct kvm_regs, ARM64_SYS_REG a system register.
#define GK_CORE_REG(x) (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | KVM_REG_ARM_CORE_REG(x))
#define GK_CORE_REG32(x) (KVM_REG_ARM64 | KVM_REG_SIZE_U32 | KVM_REG_ARM_CORE | KVM_REG_ARM_CORE_REG(x))
#define GK_SCTLR_EL1 ARM64_SYS_REG(3, 0, 1, 0, 0)
#define GK_CPACR_EL1 ARM64_SYS_REG(3, 0, 1, 0, 2)
#define GK_TTBR0_EL1 ARM64_SYS_REG(3, 0, 2, 0, 0)
#define GK_TCR_EL1 ARM64_SYS_REG(3, 0, 2, 0, 2)
#define GK_MAIR_EL1 ARM64_SYS_REG(3, 0, 10, 2, 0)
#define GK_VBAR_EL1 ARM64_SYS_REG(3, 0, 12, 0, 0)
#define GK_CNTKCTL_EL1 ARM64_SYS_REG(3, 0, 14, 1, 0)
#define GK_ID_AA64MMFR0_EL1 ARM64_SYS_REG(3, 0, 0, 7, 0)
#define GK_TPIDR_EL0 ARM64_SYS_REG(3, 3, 13, 0, 2)
#define GK_TPIDRRO_EL0 ARM64_SYS_REG(3, 3, 13, 0, 3)
#endif
// SCTLR_EL1: MMU, caches, stack alignment checks, the RES1 bits of the base
// architecture, and EL0 access to what Linux gives user code: DC ZVA (DZE),
// CTR_EL0 (UCT), cache maintenance for JIT code (UCI), WFI/WFE untrapped. No
// pointer authentication or BTI: their hint-space instructions in the host's
// code run as NOPs in the guest.
#define GK_SCTLR ((1UL << 0) | (1UL << 2) | (1UL << 3) | (1UL << 4) | (1UL << 11) | (1UL << 12) | \
                  (1UL << 14) | (1UL << 15) | (1UL << 16) | (1UL << 18) | (1UL << 20) |           \
                  (1UL << 22) | (1UL << 23) | (1UL << 26) | (1UL << 28) | (1UL << 29))
// TCR_EL1: TTBR0 walks 48-bit VAs with 4KiB pages through write-back inner-
// shareable tables, TTBR1 walks are off, ASIDs come from TTBR0 (all 0), the
// top byte is ignored for TTBR0 addresses as Linux does for user code, and
// IPS is the VM's guest-physical size (G->ips).
#define GK_TCR(ips) (16UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (16UL << 16) | (1UL << 23) | \
                     (2UL << 30) | ((uint64_t)(ips) << 32) | (1UL << 37))

#endif  // GK_ARM64_H
