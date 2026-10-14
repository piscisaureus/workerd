// gk: guest page tables: the table allocator, the supervisor/refuse registry,
// leaf mapping and range walks, and demand paging.
#include "gk_internal.h"

// Page-table page allocator over a fixed arena: a bump allocator with a free
// list in front of it, so the tables of a destroyed arena serve the next one.
// Freed pages are linked through their first word; the page is zeroed again
// when handed out. Caller holds G->lock (except in single-threaded gk_init).
static uint64_t *alloc_table(void) {
  uint64_t *p;
  if (G->pt_free) {
    p = G->pt_free;
    G->pt_free = *(uint64_t **)p;
    G->pt_free_n--;
  } else {
    if (G->pt_next + GK_GUEST_PAGE > G->pt_end) return NULL;
    p = (uint64_t *)G->pt_next;
    G->pt_next += GK_GUEST_PAGE;
  }
  memset(p, 0, GK_GUEST_PAGE);
  G->pt_used++;
  return p;
}

// Return a page-table page. The caller guarantees no root reaches it any more.
static void free_table(uint64_t *p) {
  *(uint64_t **)p = G->pt_free;
  G->pt_free = p;
  G->pt_free_n++;
  G->pt_used--;
}

// Free a table and every table beneath it. `level` is the table's paging
// level: 3 for a PDPT, 2 for a PD, 1 for a PT (whose entries are pages, not
// tables). gk maps only 4KiB pages, so every present entry above level 1
// points at a table. Caller holds G->lock.
static void free_subtree(uint64_t *tbl, int level) {
  if (level > 1)
    for (int i = 0; i < 512; i++)
      if (tbl[i] & PTE_P) free_subtree(PTE_TABLE_PTR(tbl[i]), level - 1);
  free_table(tbl);
}

static uint64_t *next_table(uint64_t *tbl, int idx) {
  if (!(tbl[idx] & PTE_P)) {
    uint64_t *n = alloc_table();
    if (!n) return NULL;
    tbl[idx] = PTE_TABLE_ENTRY(n);
    // A new base-root top-level entry: every arena root re-syncs at its next
    // entry (see G->pml4_gen). Published after the entry is in place.
    if (tbl == G->pml4) __atomic_fetch_add(&G->pml4_gen, 1, __ATOMIC_RELEASE);
  }
  return PTE_TABLE_PTR(tbl[idx]);
}

// ---- the supervisor/refuse registry ----------------------------------------
// The guest's untrusted code runs in ring 3 (see gk_run_user), so it must not
// be able to reach gk's own control structures even with arbitrary code
// execution. Every guest page falls into one of three classes, decided by
// address here and honored by map4k_root (leaf U bit) and demand_map:
//
//  KEEP (default)  the page is user-accessible (PTE_U set): the isolate arena,
//                  the runtime .text/.data and glibc, the execution stack.
//  SUPERVISOR      mapped present but with U cleared, so ring-0 (gk's fault and
//                  syscall handlers, the CPU's descriptor-table walks) reaches
//                  it while ring 3 faults: the handler text, the GDT/IDT page,
//                  and each vCPU's IST/TSS/RSP0 region.
//  REFUSE          never mapped into any guest root, so a fault there is a
//                  genuine fault even though the host has the memory: the
//                  page-table arena, each vCPU's kvm_run mmap, and the gk-loop
//                  side and host stacks. Without this, ring-3 code could fault
//                  gk's own page tables in as writable user memory and rewrite
//                  the walls.
//
// A ring's access check ANDs the U bit down the whole path, so clearing U on
// the leaf alone makes a page supervisor even though intermediate tables (which
// user pages under the same 2MB/1GB share) keep U set. Ranges are disjoint and
// address-sorted; lookups run on the fault path, so this is a static array with
// a binary-search lookup and no allocation. Guarded by G->lock.

// The class of the page holding `addr` (GK_PROT_KEEP if unregistered). Caller
// holds G->lock.
static int prot_class(uintptr_t addr) {
  int lo = 0, hi = G->prot_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (addr < G->prot[mid].start) hi = mid;
    else if (addr >= G->prot[mid].end) lo = mid + 1;
    else return G->prot[mid].kind;
  }
  return GK_PROT_KEEP;
}

// Whether the ring-3 turn on `t` may name [addr, addr+len) to a forwarded
// syscall: every host page of the range must be one ring-3 code could reach by
// its own load or store under the caller's active root, as demand_map decides.
// A page in another arena, in gk's supervisor or refuse control memory, or --
// for a write -- in the caller's host-frame wall is not reachable and fails
// the range. The host kernel still enforces its own permissions on top; this
// only keeps a forwarded syscall from reaching memory the guest itself could
// not. Empty ranges pass; a range that wraps the address space fails. Takes
// G->lock.
static int guest_range_ok(gk_thread *t, uintptr_t addr, uintptr_t len, int write) {
  if (len == 0) return 1;
  uintptr_t last = addr + (len - 1);
  if (last < addr) return 0;  // wrap
  uintptr_t p = HOST_FLOOR(addr), end = HOST_FLOOR(last);
  int ok = 1;
  pthread_mutex_lock(&G->lock);
  for (;; p += GK_HOST_PAGE) {
    gk_arena *in = arena_containing(p);
    if ((in && in != t->active_arena) || prot_class(p) != GK_PROT_KEEP ||
        (write && p >= t->ro_lo && p < t->ro_hi)) {
      ok = 0;
      break;
    }
    if (p == end) break;
  }
  pthread_mutex_unlock(&G->lock);
  return ok;
}

static void unmap_range_all(uintptr_t s, uintptr_t e);

// Register [s, e) (page-aligned) as SUPER or REFUSE, keeping the array sorted by
// start. Ranges never overlap: they are gk's own allocations. Any PTE a root
// already holds for the range is dropped, so a page that some vCPU touched
// between its allocation and this call is re-faulted under its new class
// rather than staying a user page. Caller holds G->lock (or is single-threaded
// gk_init).
//
// A full registry is fatal, never a silent no-op: an unregistered range is
// KEEP, i.e. user-accessible, so leaving a supervisor or refused range out
// would hand ring 3 the very memory (a TSS/IST, kvm_run, a host stack) the
// registry exists to keep from it. GK_MAX_PROT_RANGES is sized so that this
// cannot happen below the vCPU limit.
static void prot_add(uintptr_t s, uintptr_t e, int kind) {
  if (G->prot_n >= GK_MAX_PROT_RANGES)
    gk_fatal("supervisor/refuse registry full; cannot leave a protected range user-mapped");
  int i = 0;
  while (i < G->prot_n && G->prot[i].start < s) i++;
  memmove(&G->prot[i + 1], &G->prot[i], (size_t)(G->prot_n - i) * sizeof G->prot[0]);
  G->prot[i] = (gk_prot_range){s, e, kind};
  G->prot_n++;
  if (G->pml4) unmap_range_all(s, e);
}

// Unregister the range [s, e) registered by prot_add, when the memory behind
// it is being given back (a guest thread's host stack at its exit), so that
// whatever the address is later reused for is classified afresh. Caller holds
// G->lock.
static void prot_remove(uintptr_t s, uintptr_t e) {
  for (int i = 0; i < G->prot_n; i++) {
    if (G->prot[i].start != s || G->prot[i].end != e) continue;
    memmove(&G->prot[i], &G->prot[i + 1], (size_t)(G->prot_n - i - 1) * sizeof G->prot[0]);
    G->prot_n--;
    return;
  }
}

// ---- global pages ----------------------------------------------------------
// The guest runs with CR4.PGE, so a PTE carrying the G bit yields a TLB entry
// that a CR3 load keeps. A turn on a thread whose previous turn ran another
// isolate loads that isolate's root at entry (see enter_guest); without global
// pages that load drops every translation the vCPU had, and the turn starts
// cold on the shared runtime's text, data and heap. With them, only the
// previous arena's translations go.
//
// A global entry is correct only if the translation it caches is the same
// under every root the vCPU can ever load, and stays so until a flush that
// includes global entries. Both hold, by construction, for the pages that get
// PTE_G, and map4k_root sets it for no others:
//
//  * The PTE lives in a table every root shares. PTE_G goes only into the base
//    root, for a page outside every arena's reservation. Such a PTE sits under
//    a base-root PML4 entry, which every arena root copies (arena_sync_root)
//    and which is never replaced once set (the shared subtree is never freed),
//    so every root reaches the identical PTE or, until its next sync, none. An
//    arena's private subtree is never global: it differs per root by design,
//    and a surviving entry would let one isolate's turn reach another's memory
//    after the switch.
//  * The page's address can never later become arena memory. A slot that has
//    ever held a global PTE is pinned (slot_pinned) and gk_arena_create never
//    places an arena over it; otherwise a stale global translation of a runtime
//    mapping that was later unmapped could outlive the slot's reuse as an
//    arena, and a turn for another arena could read that arena's memory through
//    it. Pinning changes nothing in practice: an arena needs its whole 512GiB
//    span free, and the slots holding the runtime's mappings never are.
//  * The page is an ordinary user page (KEEP) -- supervisor pages stay
//    non-global as a matter of caution, not need -- and not part of a thread
//    stack: not in the calling vCPU's host-frame wall window, not in the stack
//    mapping its ring-3 turns run on, and not in the mapping holding the
//    interrupted code's stack pointer (see demand_map; the eager-mapped guest
//    and child stacks never get the flag either). The wall withholds W on a
//    window page per turn; with the stack non-global, a root switch between
//    turns drops the vCPU's stack translations regardless, so wall_raise's
//    flush-skip argument only ever has to carry turns with no switch between.
//
// Every TLB flush gk does itself includes global entries: the flush stub
// (see flush_run) runs INVPCID type 2 or toggles CR4.PGE, either of which the
// CPU defines to drop every entry, global or not.
// So a reflected mprotect/munmap, a wall raise and the root_gen flush all drop
// them, and a changed shared PTE never leaves a stale global entry on the vCPU
// that changed it; other vCPUs are not shot down, with or without global pages
// (see the README's limitations). The per-turn root load is the guest's own
// MOV CR3 (gk_launch_cr3, gk_user_launch_cr3), which keeps global entries; a
// root load through KVM (cr3_load_host, host_backing_retry) makes KVM flush the
// whole guest TLB, global entries included, which is harmless.

// Four-level map of one page; guest-virtual == guest-physical == host-virtual.
// `pkey` (0..15) is the page's protection key, placed in PTE bits 62:59; the
// guest CPU checks it against the guest PKRU on every data access (with
// CR4.PKE and CR0.WP set, see vcpu_init), which is what makes the guest's own
// protection keys real. Key 0 is the default, unrestricted key.
//
// The leaf's U bit follows the supervisor/refuse registry: a SUPER page is
// mapped with U cleared (ring 3 cannot reach it), a REFUSE page is never mapped
// at all, and everything else is user-accessible. GK_MAP_GLOBAL asks for the G
// bit; it is granted only under the conditions of the global-pages section,
// which are checked here again rather than trusted from the caller. Caller
// holds G->lock.
static int map4k_root(uint64_t *root, uint64_t va, uint64_t flags, int pkey) {
  int cls = prot_class(va & ~0xfffULL);
  if (cls == GK_PROT_REFUSE) return -1;  // a control page: never reachable
  int slot = (int)((va >> 39) & 0x1ff);
  int global = (flags & GK_MAP_GLOBAL) && root == G->pml4 && cls == GK_PROT_KEEP &&
               !(flags & GK_MAP_WALLED) && slot < GK_USER_SLOTS && !arena_containing(va);
  uint64_t *pdpt = next_table(root, (va >> 39) & 0x1ff);
  if (!pdpt) return -1;
  uint64_t *pd = next_table(pdpt, (va >> 30) & 0x1ff);
  if (!pd) return -1;
  uint64_t *pt = next_table(pd, (va >> 21) & 0x1ff);
  if (!pt) return -1;
  // `flags` is a protection bitmask: 1=read, 2=write, 4=execute. Honor it so
  // W^X holds: code is mapped executable but not writable, data writable but
  // not executable. KVM also enforces the host VMA's real protection.
  // GK_MAP_WALLED marks a page the host allows writes to whose W this vCPU's
  // host-frame wall withholds: it is mapped without W and tagged PTE_WALL so
  // wall_lower can give W back in place.
  uint64_t pte;
  if (pte_leaf(va, flags, pkey, cls == GK_PROT_SUPER, &pte) < 0) return -1;
  if (global) {
    pte = PTE_SET_GLOBAL(pte);
    G->slot_pinned[slot] = 1;  // no arena may ever take this slot (see the global-pages section)
    G->global_pages++;
  }
  pt[(va >> 12) & 0x1ff] = pte;
  return 0;
}


// Apply `op` to the leaf PTEs of [s, e) (page-aligned) in a root. Walks the
// hierarchy and skips a whole 512GiB, 1GiB or 2MiB range at once where no
// table exists beneath it, so a walk over a large, sparsely committed
// reservation costs in proportion to what is mapped, not to the range. Leaves
// intermediate tables in place and never allocates. `rp` is PTE_OP_REPROTECT's
// argument and NULL for the other ops. Returns PTE_OP_WALL_LOWER's count of
// untagged writable entries, 0 for the other ops. Caller holds G->lock.
#define GK_NEXT_BOUNDARY(v, bits) ((((v) >> (bits)) + 1) << (bits))
static long pte_range_root(uint64_t *root, uintptr_t s, uintptr_t e, int op,
                           const gk_reprot *rp) {
  long n = 0;
  for (uintptr_t v = s; v < e;) {
    uint64_t pml4e = root[(v >> 39) & 0x1ff];
    if (!(pml4e & PTE_P)) { v = GK_NEXT_BOUNDARY(v, 39); continue; }
    uint64_t *pdpt = PTE_TABLE_PTR(pml4e);
    uint64_t pdpte = pdpt[(v >> 30) & 0x1ff];
    if (!(pdpte & PTE_P)) { v = GK_NEXT_BOUNDARY(v, 30); continue; }
    uint64_t *pd = PTE_TABLE_PTR(pdpte);
    uint64_t pde = pd[(v >> 21) & 0x1ff];
    if (!(pde & PTE_P)) { v = GK_NEXT_BOUNDARY(v, 21); continue; }
    uint64_t *pt = PTE_TABLE_PTR(pde);
    uint64_t *pte = &pt[(v >> 12) & 0x1ff];
    switch (op) {
      case PTE_OP_UNMAP:
        *pte = 0;
        break;
      case PTE_OP_WALL_RAISE:
        if ((*pte & PTE_P) && PTE_IS_W(*pte)) *pte = PTE_CLR_W(*pte) | PTE_WALL;
        break;
      case PTE_OP_WALL_LOWER:
        if ((*pte & (PTE_P | PTE_WALL)) == (PTE_P | PTE_WALL)) *pte = PTE_SET_W(*pte & ~PTE_WALL);
        else if ((*pte & PTE_P) && PTE_IS_W(*pte)) n++;
        break;
      case PTE_OP_REPROTECT:
        // Only W, NX and the wall tag change; the frame, P, U (the
        // supervisor/refuse class), the protection key and the accessed/dirty
        // bits are the page's own and stay.
        if (*pte & PTE_P) {
          *pte = pte_reprotect(*pte, rp, v);
        }
        break;
    }
    v += 0x1000;
  }
  return n;
}

// The leaf PTE a root holds for the page of `va`, or 0 if no table on the way
// down exists. Never allocates. Caller holds G->lock.
static uint64_t pte_lookup(const uint64_t *root, uintptr_t va) {
  uint64_t pml4e = root[(va >> 39) & 0x1ff];
  if (!(pml4e & PTE_P)) return 0;
  const uint64_t *pdpt = PTE_TABLE_PTR(pml4e);
  uint64_t pdpte = pdpt[(va >> 30) & 0x1ff];
  if (!(pdpte & PTE_P)) return 0;
  const uint64_t *pd = PTE_TABLE_PTR(pdpte);
  uint64_t pde = pd[(va >> 21) & 0x1ff];
  if (!(pde & PTE_P)) return 0;
  const uint64_t *pt = PTE_TABLE_PTR(pde);
  return pt[(va >> 12) & 0x1ff];
}


// Apply `op` over [s, e) in every root that may hold PTEs for it: the base
// root (whose subtrees every arena root shares for addresses outside arenas)
// and each arena whose reservation the range overlaps (their private subtrees
// are reachable from no other root). Returns the sum of the walks' counts
// (see pte_range_root). Caller holds G->lock.
static long pte_range_all(uintptr_t s, uintptr_t e, int op, const gk_reprot *rp) {
  long n = pte_range_root(G->pml4, s, e, op, rp);
  for (int i = 0; i < G->arena_n; i++) {
    gk_arena *a = G->arenas[i];
    uintptr_t lo = s > a->base ? s : a->base, hi = e < a->end ? e : a->end;
    if (lo < hi) n += pte_range_root(a->pml4, lo, hi, op, rp);
  }
  return n;
}

// Reflect a host protection or mapping change over [s, e) by dropping every
// PTE any root holds for it (see pte_range_all), so the guest's next access
// demand-faults and re-derives the mapping from the host. A stale PTE over a
// page the host has decommitted would otherwise make that access an
// unrecoverable KVM_RUN EFAULT rather than a demand fault. Caller holds
// G->lock.
static void unmap_range_all(uintptr_t s, uintptr_t e) {
  pte_range_all(s, e, PTE_OP_UNMAP, NULL);
}

// Reflect a successful plain mprotect over [s, e) that leaves the range
// readable by rewriting, in place, every PTE any root holds for it (see
// pte_range_all) to `prot`, the syscall's own argument: with the host call
// just made under G->lock, that is exactly the host protection of every page
// in the range, so nothing is cached and nothing needs re-deriving. Pages the
// guest has no PTE for yet are untouched and fault in later against the host
// as always. Dropping the PTEs instead (unmap_range_all) would have each one
// re-fault, and every demand fault reads /proc/self/maps, which is long in
// this process. `t` is the calling vCPU, whose host-frame wall the rewrite
// honors (see gk_reprot). Caller holds G->lock.
static void reprotect_range_all(gk_thread *t, uintptr_t s, uintptr_t e, int prot) {
  gk_reprot rp = {prot, t->ro_lo, t->ro_hi};
  pte_range_all(s, e, PTE_OP_REPROTECT, &rp);
}

// The same for one root only: an arena root being built or torn down, whose
// private subtree no other root reaches. Caller holds G->lock.
static void unmap_range_root(uint64_t *root, uintptr_t s, uintptr_t e) {
  pte_range_root(root, s, e, PTE_OP_UNMAP, NULL);
}


// Eager-map [s, e) into the base root with PTE protection `perms`, backing the
// containing GK_BACK_WIN windows so later demand faults in the same region find
// an existing memslot. Refused pages within the range are left unmapped, as a
// demand fault would leave them: a host mapping that abuts one of gk's refused
// allocations (the kernel merges neighboring anonymous mappings) can be
// eager-mapped without the refused part failing the whole call. So are pages
// inside an arena's reservation (a guest-created thread whose clone named a
// stack there, say): they belong to the arena's private subtree and are left
// to demand paging under the arena's root. Installed here they would land in
// the base root's subtree for the arena's slot, which every other root shares,
// and be reachable from every other arena. Nothing mapped here is global: the
// ranges are gk's control pages and guest stacks. Caller holds G->lock.
static int mmu_map_range(uintptr_t s, uintptr_t e, int perms) {
  if (region_ensure(s, e) < 0) return -1;
  for (uintptr_t v = GUEST_FLOOR(s); v < e; v += GK_GUEST_PAGE) {
    if (prot_class(v) == GK_PROT_REFUSE || arena_containing(v)) continue;
    if (map4k_root(G->pml4, v, (uint64_t)perms, pkey_lookup(v)) < 0) return -1;
  }
  return 0;
}

// Map the pages around a just-mapped user page ahead of their own faults, from
// the same /proc/self/maps read that mapped it. That read is the bulk of a
// demand fault's cost (the kernel renders every mapping of a V8-sized process
// up to the one asked for), and it describes the whole host mapping [rs, re)
// with `perms`, so one read can settle a neighborhood of pages rather than
// one. The neighborhood is the GK_NEIGHBOR_WIN-aligned window holding `page`,
// clipped to the host mapping: it is aligned, so it never leaves the 2MB
// backing window of `page` or crosses a 512GiB slot, and everything in it that
// gets a PTE is host-backed at exactly `perms` under the same G->lock hold
// that read them. The window is 256KB, the chunk V8 grows its regular heap
// by (adjacent chunks merge into one host mapping, so the mapping's own size
// says nothing about the growth step): a chunk then costs one read as its
// pages are first touched, not one per 64KB. Nothing here is cached beyond
// the read: a later host change is reflected or recovered for these PTEs
// exactly as for a page the guest faulted itself (unmap_range_all,
// reprotect_range_all, host_backing_retry).
//
// This is an optimization, never a widening: a page is mapped only when it
// would have been mapped the same way by its own fault under demand_map's
// rules, and skipped otherwise, to fault on its own later.
//  * Its class is KEEP: SUPER and REFUSE pages are left to their own faults
//    (a REFUSE page is refused there; a SUPER page is mapped with U cleared).
//  * It lies in the same arena as `page`, or like `page` in none, so it goes
//    into the same root: an arena page never lands in the base root, and no
//    other arena's page is mapped at all. (Aligned windows cannot straddle an
//    arena boundary anyway; the check is per page regardless.)
//  * Its backing window has a memslot; none is created here.
//  * It has no PTE in the root yet: an existing entry may be carrying wall or
//    protection state that a reflection maintains in place, and is left alone.
//  * It is in no other vCPU's host-frame wall window. Such a page is that
//    vCPU's caller stack, which only that vCPU's own faults should map, and
//    they map it without W. A page in this vCPU's own window is mapped the
//    way its own read fault would be: without W, tagged PTE_WALL.
//  * Its protection key is looked up per page, like the global-pages
//    decision, which map4k_root re-checks itself.
// Faults counted in demand_faults are the guest's own; pages mapped here are
// counted in neighbor_pages. Caller holds G->lock; `root`, `in`, `rs`, `re`,
// `perms` and `sp` are demand_map's for the page it just mapped.
static void demand_map_neighbors(gk_thread *t, uintptr_t page, uint64_t *root, gk_arena *in,
                                 uintptr_t rs, uintptr_t re, int perms, uintptr_t sp) {
  uintptr_t lo = page & ~(GK_NEIGHBOR_WIN - 1), hi = lo + GK_NEIGHBOR_WIN;
  if (lo < rs) lo = rs;
  if (hi > re) hi = re;
  // The mapping is the interrupted code's stack, or this vCPU's ring-3 stack:
  // the same for every page of it (see demand_map).
  int stack_map = (rs < t->wall_map_hi && re > t->wall_map_lo) || (sp >= rs && sp < re);
  for (uintptr_t v = lo; v < hi; v += GK_GUEST_PAGE) {
    if (v == page) continue;
    if (prot_class(v) != GK_PROT_KEEP) continue;
    if (arena_containing(v) != in) continue;
    if (!region_find(v)) continue;
    if (pte_lookup(root, v) & PTE_P) continue;
    int foreign_wall = 0;
    for (int i = 0; i < G->thread_n && !foreign_wall; i++) {
      gk_thread *o = &G->threads[i];
      if (o != t && v >= o->ro_lo && v < o->ro_hi) foreign_wall = 1;
    }
    if (foreign_wall) continue;
    int walled = v >= t->ro_lo && v < t->ro_hi;
    uint64_t flags = (uint64_t)perms;
    if (walled && (perms & 2)) flags = (flags & ~2ULL) | GK_MAP_WALLED;
    if (!in && !walled && !stack_map) flags |= GK_MAP_GLOBAL;
    if (map4k_root(root, v, flags, pkey_lookup(v)) < 0) continue;
    G->neighbor_pages++;
  }
}

// Handle a guest page fault: if the faulting page is host-accessible, back it
// with a memslot and PTE so the guest can retry. Caller must not hold G->lock.
//
// The PTE always carries the page's *current* host protection, re-read from
// /proc/self/maps on every fault, so mprotect/mmap protection changes are
// honored even if a syscall reflection missed them; a stale read-only PTE on a
// page the host has made writable would silently drop guest writes. The host
// protection is read under G->lock, the same lock a protection-changing syscall
// holds across its host call and PTE clearing (see forward_syscall), so another
// vCPU cannot change a page's protection between this read and the PTE install.
//
// Which root gets the PTE depends on where the page lies. Inside this vCPU's
// active arena, the PTE goes under that arena's private subtree, so the page
// is reachable from that root only; the host protection re-read below is what
// the arena's owner committed the page with (uncommitted pages are PROT_NONE
// and refused). Inside any other arena the fault is refused outright: that is
// a cross-arena access, and it must stay unaddressable even though the host
// could back it. Outside every arena the PTE goes into the base root, whose
// subtrees all arena roots share, and is global unless the page is part of a
// stack (see the global-pages section); `sp` is the interrupted code's stack
// pointer, which names the mapping its stack lies in.
static int demand_map(gk_thread *t, uintptr_t addr, uintptr_t sp) {
  uintptr_t page = GUEST_FLOOR(addr);
  uintptr_t rs, re; int perms;
  pthread_mutex_lock(&G->lock);
  uint64_t *root = G->pml4;
  gk_arena *in = arena_containing(page);
  if (in) {
    if (in != t->active_arena) {
      pthread_mutex_unlock(&G->lock);
      return -1;  // another arena's memory: refuse, whatever the host has there
    }
    root = in->pml4;
  }
  // Consult the supervisor/refuse registry. A REFUSE page (gk's page tables,
  // kvm_run, the gk-loop stacks) is never mapped, so a fault there is genuine
  // even though the host has the memory: this is what stops ring-3 code from
  // faulting gk's own control memory in as ordinary user pages. A SUPER page
  // (handler text, GDT/IDT, IST/TSS) is mapped with U cleared by map4k_root.
  int cls = prot_class(page);
  if (cls == GK_PROT_REFUSE) {
    if (G->dbg) {
      gk_dbuf d = {.n = 0};
      db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
      db_str(&d, ": REFUSE fault "); db_hex(&d, page);
      db_str(&d, " (gk control region)");
      db_flush(&d);
    }
    pthread_mutex_unlock(&G->lock);
    return -1;
  }
  int mapped = host_region(page, &rs, &re, &perms);
  if (!mapped && host_stack_grow(page)) mapped = host_region(page, &rs, &re, &perms);
  if (!mapped || !(perms & 1)) {
    pthread_mutex_unlock(&G->lock);
    return -1;  // not host-readable: a genuine fault
  }
  if (!region_find(page) && region_ensure(page, page + 1) < 0) {
    pthread_mutex_unlock(&G->lock);
    return -1;
  }
  int pkey = pkey_lookup(page);
  // Inside this vCPU's host-frame wall the page is mapped without write
  // permission whatever the host has: readable (a conservative GC scan reads
  // the caller's frames) but not writable by the turn. A write fault there
  // never reaches this point (see run_vcpu), so only reads are mapped. A page
  // the host does allow writes to is tagged PTE_WALL, so that wall_lower can
  // give W back in place instead of the page re-faulting after the turn.
  uint64_t flags = (uint64_t)perms;
  if (page >= t->ro_lo && page < t->ro_hi && (perms & 2)) flags = (flags & ~2ULL) | GK_MAP_WALLED;
  // A base-root page may be global unless it is part of a stack: in this
  // vCPU's wall window, in the stack mapping its ring-3 turns run on (cached
  // by stack_wall_bounds; the mapping may have grown since, hence the overlap
  // test), or in the mapping the interrupted code's stack pointer lies in.
  // map4k_root checks the rest of the global-pages conditions itself.
  int stack = (page >= t->ro_lo && page < t->ro_hi) ||
              (rs < t->wall_map_hi && re > t->wall_map_lo) || (sp >= rs && sp < re);
  if (!in && !stack) flags |= GK_MAP_GLOBAL;
  int r = map4k_root(root, page, flags, pkey);  // honor R/W/X for W^X
  if (r >= 0) {
    G->demand_ok++;
    // Settle the page's neighborhood from the same read (an aligned window,
    // so it shares the page's top-level entry, which the carry-over below
    // covers).
    if (cls == GK_PROT_KEEP) demand_map_neighbors(t, page, root, in, rs, re, perms, sp);
    // A base-root mapping that populated a fresh top-level entry is not yet in
    // the active arena's root (which copied the base root's entries at entry,
    // see sync_cr3); carry it over now so the retry does not fault again. The
    // index cannot be one of the arena's own slots: the page is outside every
    // arena.
    gk_arena *a = t->active_arena;
    if (root == G->pml4 && a) {
      int idx = (int)((page >> 39) & 0x1ff);
      if (a->pml4[idx] != G->pml4[idx]) a->pml4[idx] = G->pml4[idx];
    }
  }
  pthread_mutex_unlock(&G->lock);
  if (G->dbg && (cls == GK_PROT_SUPER || pkey != 0)) {
    gk_dbuf d = {.n = 0};
    db_str(&d, "[gk] vcpu "); db_dec(&d, t->id);
    db_str(&d, cls == GK_PROT_SUPER ? ": SUPER demand-map " : ": demand-map ");
    db_hex(&d, page);
    db_str(&d, " perms="); db_dec(&d, perms);
    if (cls == GK_PROT_SUPER) db_str(&d, " (U cleared)");
    if (pkey != 0) { db_str(&d, " pkey="); db_dec(&d, pkey); db_str(&d, " (PTE bits 62:59)"); }
    db_flush(&d);
  }
  return r;
}
