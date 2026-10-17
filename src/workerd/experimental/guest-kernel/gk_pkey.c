// gk: the guest's memory protection keys: the per-range key table.
#include "gk_internal.h"

// ---- protection keys ---------------------------------------------------------
// The guest's memory protection keys are virtualized, not emulated: the guest
// runs with CR4.PKE, so its rdpkru/wrpkru operate on the vCPU's own PKRU (KVM
// saves and restores it around every exit), and every guest PTE carries the
// key its page was assigned with pkey_mprotect (map4k_root). The guest CPU then
// enforces the key on every data access exactly as it would natively, and a
// violation arrives as a #PF with the PK error bit (see run_vcpu).
//
// The host side is deliberately kept out of it. A pkey_mprotect is issued on
// the host as a plain mprotect, so no host VMA ever carries a key and KVM's
// page backing (get_user_pages, which checks the host thread's PKRU against the
// host VMA's key) can never pkey-fault whatever the guest has put in its PKRU.
// Only the guest enforces; the host's PKRU is irrelevant to correctness (see
// host_pkru_allow_all).
//
// The key of each range is tracked here, in a sorted array of disjoint
// [start, end) ranges holding a nonzero key (key 0, the default, is implicit).
// Linux semantics are followed: pkey_mprotect with an explicit key sets it, a
// plain mprotect (or pkey -1) keeps the range's key, munmap and mmap drop it
// (a fresh mapping has key 0), and pkey_free leaves it in the PTEs. Lookups
// run on the fault path (demand_map), so the table is static and the lookup a
// binary search: no allocation there. Guarded by G->lock.
//
// Which keys a pkey_mprotect may use is not checked against the set of keys
// the process has allocated: that set is the kernel's, and gk sees only the
// pkey_alloc calls forwarded from inside the guest, not the ones the host side
// makes outside any guest entry (V8 allocates its sandbox, JIT and pointer-
// table keys at initialization, and keys pages with them from within a turn
// whenever it commits or decommits sandbox, code or table pages). A key the
// kernel would reject as unallocated is thus virtualized like any other; it
// costs nothing in isolation, which comes from the page-table root, and the
// guest can only ever key pages it could mprotect anyway. What gk cannot do
// -- reflect a key outside the PTE field, or record one more range -- fails
// with ENOMEM, the one errno a pkey_mprotect caller has to be prepared for
// (V8 aborts on any other).
//
// Master switch for protection-key virtualization. gk isolates isolates by
// page-table root, so V8's host-side keys are not needed for security in this
// model; set this to 0 and the guest PTEs carry no key, so nothing is enforced
// and the keys become no-ops -- turning the whole scheme off in one line while
// leaving the rest (W^X, CR0.WP, the real TLB flush) intact.
#ifndef GK_VIRTUALIZE_PKEYS
#define GK_VIRTUALIZE_PKEYS 1
#endif


// The key of the page holding `addr`, or 0. Caller holds G->lock.
static int pkey_lookup(uintptr_t addr) {
  if (!GK_VIRTUALIZE_PKEYS) return 0;  // keys not reflected into PTEs -> no-ops
  int lo = 0, hi = G->pkey_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (addr < G->pkeys[mid].start) hi = mid;
    else if (addr >= G->pkeys[mid].end) lo = mid + 1;
    else return G->pkeys[mid].pkey;
  }
  return 0;
}

// Index of the first range whose end is beyond `addr`. Caller holds G->lock.
static int pkey_lower_bound(uintptr_t addr) {
  int lo = 0, hi = G->pkey_n;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (G->pkeys[mid].end <= addr) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// Set the key of [s, e) (page-aligned) to `pkey`, replacing whatever keys the
// range held. Existing ranges overlapping [s, e) are trimmed, split or removed;
// a nonzero key is then inserted and merged with equal-key neighbors. Fails
// only when the table is full (pkey_room() guarantees it is not); the table is
// then left unchanged. Caller holds G->lock.
static int pkey_room(void) { return G->pkey_n + 2 <= GK_MAX_PKEY_RANGES; }
static int pkey_set_range(uintptr_t s, uintptr_t e, int pkey) {
  if (!pkey_room()) return -1;
  int i = pkey_lower_bound(s);
  while (i < G->pkey_n && G->pkeys[i].start < e) {
    gk_pkey_range *r = &G->pkeys[i];
    if (r->start < s && r->end > e) {  // [s, e) is strictly inside r: split it
      memmove(&G->pkeys[i + 2], &G->pkeys[i + 1], (size_t)(G->pkey_n - i - 1) * sizeof *r);
      G->pkey_n++;
      G->pkeys[i + 1] = (gk_pkey_range){e, r->end, r->pkey};
      r->end = s;
      i++;
      break;
    } else if (r->start < s) {  // r sticks out to the left: keep that part
      r->end = s;
      i++;
    } else if (r->end > e) {  // r sticks out to the right: keep that part
      r->start = e;
      break;
    } else {  // r lies within [s, e): drop it
      memmove(r, r + 1, (size_t)(G->pkey_n - i - 1) * sizeof *r);
      G->pkey_n--;
    }
  }
  if (pkey == 0) return 0;
  // Now every range before index i ends at or before s, and every range from i
  // on starts at or after e. Insert [s, e), absorbing adjacent equal keys.
  if (i > 0 && G->pkeys[i - 1].end == s && G->pkeys[i - 1].pkey == pkey) {
    G->pkeys[i - 1].end = e;
    if (i < G->pkey_n && G->pkeys[i].start == e && G->pkeys[i].pkey == pkey) {
      G->pkeys[i - 1].end = G->pkeys[i].end;
      memmove(&G->pkeys[i], &G->pkeys[i + 1], (size_t)(G->pkey_n - i - 1) * sizeof G->pkeys[0]);
      G->pkey_n--;
    }
    return 0;
  }
  if (i < G->pkey_n && G->pkeys[i].start == e && G->pkeys[i].pkey == pkey) {
    G->pkeys[i].start = s;
    return 0;
  }
  memmove(&G->pkeys[i + 1], &G->pkeys[i], (size_t)(G->pkey_n - i) * sizeof G->pkeys[0]);
  G->pkeys[i] = (gk_pkey_range){s, e, pkey};
  G->pkey_n++;
  return 0;
}
