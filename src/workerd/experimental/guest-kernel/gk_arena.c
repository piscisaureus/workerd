// gk: per-isolate arenas.
#include "gk_internal.h"

static gk_arena *arena_containing(uintptr_t a) {
  for (int i = 0; i < G->arena_n; i++)
    if (a >= G->arenas[i]->base && a < G->arenas[i]->end) return G->arenas[i];
  return NULL;
}

// ---- arenas ----------------------------------------------------------------
// The reservation must own whole PML4 slots: an arena root's private entry for
// a slot hides everything else in that 512GiB from the arena, so no other
// mapping may share it. The kernel would place a free-address mmap next to the
// libraries and stacks at the top of the address space, in a slot the whole
// process lives in; instead the span is asked for at a slot-aligned address
// with MAP_FIXED_NOREPLACE, which succeeds only if the entire span is free, and
// the next slots are tried when it is not. The reservation is PROT_NONE and
// MAP_NORESERVE: it costs address space only, and its pages stay unmapped in
// every root until the owner commits them and the guest touches them.
//
// Slots are taken from the lowest run of nslots that no live arena owns and
// that has never held a global PTE (see the global-pages section), so a
// destroyed arena's slots serve later arenas: under isolate churn the slot
// index would otherwise run off the end of the user address space. For that
// to hold, a destroyed arena's span is not unmapped but replaced by a fresh
// reservation that gk keeps (slot_held): a free-address mmap of the process's
// own, or of a system library's (Hypervisor.framework allocates its vCPU
// pages wherever the kernel finds room), could otherwise land in a freed slot
// and keep every later arena out of it. A run is taken over held slots as
// they are and over the others with MAP_FIXED_NOREPLACE.


// Reserve the run of nslots slots from idx, none of them used or pinned: a
// held slot is gk's already, and any other must be free host address space,
// which MAP_FIXED_NOREPLACE takes only if nothing is mapped there. On success
// the whole run is reserved PROT_NONE and none of it is held any more (the
// caller takes it); on failure the slots this call reserved are released and
// the held ones stay held. Caller holds G->lock.
static int reserve_slots(int idx, int nslots) {
  int i;
  for (i = 0; i < nslots; i++) {
    if (G->slot_held[idx + i]) continue;
    uintptr_t va = (uintptr_t)(idx + i) << GK_SLOT_BITS;
    void *m = mmap((void *)va, GK_SLOT_BYTES, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (m == MAP_FAILED) break;
    if ((uintptr_t)m != va) { munmap(m, GK_SLOT_BYTES); break; }  // a kernel that only hints
  }
  if (i < nslots) {
    while (--i >= 0)
      if (!G->slot_held[idx + i]) munmap((void *)((uintptr_t)(idx + i) << GK_SLOT_BITS), GK_SLOT_BYTES);
    return -1;
  }
  for (i = 0; i < nslots; i++) G->slot_held[idx + i] = 0;
  return 0;
}

gk_arena *gk_arena_create(size_t size) {
  size = HOST_CEIL(size);  // the owner commits it with host mprotects
  if (size == 0) return NULL;
  int nslots = (int)((size + GK_SLOT_BYTES - 1) >> GK_SLOT_BITS);
  if (nslots > GK_MAX_ARENA_SLOTS) return NULL;
  size_t span = (size_t)nslots << GK_SLOT_BITS;
  pthread_mutex_lock(&G->lock);
  // The struct comes from the control block's pool: its root pointer and slot
  // entries are the arena's walls, so it must be as unreachable to ring 3 as
  // the page tables themselves.
  gk_arena *a = G->arena_free;
  if (a) G->arena_free = a->next_free;
  else if (G->arena_pool_n < GK_MAX_ARENAS) a = &G->arena_pool[G->arena_pool_n++];
  if (!a) { pthread_mutex_unlock(&G->lock); return NULL; }
  memset(a, 0, sizeof *a);
  void *mem = MAP_FAILED;
  int idx;
  for (idx = GK_FIRST_CAGE_SLOT; idx + nslots <= GK_USER_SLOTS; idx++) {
    int free = 1;
    for (int i = 0; i < nslots; i++)
      if (G->slot_used[idx + i] || G->slot_pinned[idx + i]) { free = 0; break; }
    if (!free) continue;
    if (reserve_slots(idx, nslots) == 0) { mem = (void *)((uintptr_t)idx << GK_SLOT_BITS); break; }
  }
  if (mem == MAP_FAILED || G->arena_n >= GK_MAX_ARENAS) goto fail;
  a->base = (uintptr_t)mem;
  a->size = size;
  a->end = a->base + span;
  a->slot0 = idx;
  a->nslots = nslots;
  a->pml4 = alloc_table();
  if (!a->pml4) goto fail;
  // One private PDPT per slot, allocated now so the entry never changes; the
  // PD and PT pages beneath it are allocated as pages demand-fault in.
  for (int i = 0; i < nslots; i++) {
    uint64_t *pdpt = alloc_table();
    if (!pdpt) goto fail;
    a->slot_entry[i] = PTE_TABLE_ENTRY(pdpt);
  }
  // The span was free host address space, so any base-root PTE in it (from
  // memory that once lived there) is stale; drop them so nothing maps into
  // the reservation from outside the arena.
  unmap_range_root(G->pml4, a->base, a->end);
  arena_sync_root(a);  // share the base root's other entries, own these slots
  a->synced_gen = G->pml4_gen;  // under G->lock: no entry can be added in between
  for (int i = 0; i < nslots; i++) G->slot_used[idx + i] = 1;
  G->arenas[G->arena_n++] = a;
  pthread_mutex_unlock(&G->lock);
  return a;
fail:
  for (int i = 0; i < GK_MAX_ARENA_SLOTS; i++)
    if (a->slot_entry[i]) free_table(PTE_TABLE_PTR(a->slot_entry[i]));
  if (a->pml4) free_table(a->pml4);
  a->next_free = G->arena_free;
  G->arena_free = a;
  if (mem != MAP_FAILED)  // the run stays reserved, for the next arena
    for (int i = 0; i < nslots; i++) G->slot_held[idx + i] = 1;
  pthread_mutex_unlock(&G->lock);
  return NULL;
}

void *gk_arena_base(const gk_arena *a) { return a ? (void *)a->base : NULL; }
size_t gk_arena_size(const gk_arena *a) { return a ? a->size : 0; }

gk_arena *gk_arena_enter(gk_arena *a) {
  gk_thread *t = thread_get();
  if (!t) return NULL;  // record pool exhausted: the thread stays on the base root
  return arena_enter(t, a);
}

// Teardown reconciles gk and KVM with the reservation going away, in this
// order, under G->lock:
//  1. The arena leaves the registry, so a fault in its span is no longer an
//     arena fault, and its span loses its protection keys.
//  2. Every root stops mapping the span: the arena's private subtrees are
//     freed whole (no other root reaches them), and any stale base-root PTEs
//     in the span are cleared (other arena roots share those tables).
//  3. Every memslot in the span is deleted. Nothing maps into the span at
//     this point, so no vCPU can reach a memslot as it goes.
// Only then, outside the lock, is the arena's memory replaced by a fresh
// reservation, so KVM never holds a memslot over host memory that is gone
// while a guest PTE could still lead there. Its slots are released for the
// next arena after that, held by gk (see gk_arena_create): released any
// earlier, a concurrent gk_arena_create could take them and have its
// reservation replaced. The page-table pages return to the allocator and the
// slots and memslot ids are reused, so arena churn is bounded in every
// resource.
//
// A thread that still has the arena active (a caller bug: the arena's memory
// is being unmapped under it) keeps a root that must stay intact and private,
// so in that case the tables, slots and struct are leaked instead of recycled;
// the memslots go and the span is unmapped.
void gk_arena_destroy(gk_arena *a) {
  if (!a) return;
  gk_thread *t = thread_cur();
  if (t && t->active_arena == a) arena_enter(t, NULL);
  pthread_mutex_lock(&G->lock);
  for (int i = 0; i < G->arena_n; i++)
    if (G->arenas[i] == a) { G->arenas[i] = G->arenas[--G->arena_n]; break; }
  if (pkey_set_range(a->base, a->end, 0) < 0 && G->dbg)
    fprintf(stderr, "[gk] pkey table full; destroyed arena keeps stale keys\n");
  int in_use = __atomic_load_n(&a->active_threads, __ATOMIC_SEQ_CST);
  if (in_use > 0) {
    fprintf(stderr, "[gk] gk_arena_destroy: arena [%#lx,%#lx) is still active on %d thread(s); "
            "its page tables and slots are leaked\n", (unsigned long)a->base,
            (unsigned long)a->end, in_use);
  } else {
    for (int i = 0; i < a->nslots; i++) {
      free_subtree(PTE_TABLE_PTR(a->slot_entry[i]), 3);
      a->slot_entry[i] = 0;
      a->pml4[a->slot0 + i] = 0;
    }
    free_table(a->pml4);
    __atomic_fetch_add(&G->root_gen, 1, __ATOMIC_RELEASE);
  }
  unmap_range_root(G->pml4, a->base, a->end);
  int removed = region_remove_range(a->base, a->end, in_use > 0);
  uintptr_t base = a->base, end = a->end;
  int slot0 = a->slot0, nslots = a->nslots;
  if (in_use == 0) {
    a->next_free = G->arena_free;  // the struct is the pool's again
    G->arena_free = a;
  }
  pthread_mutex_unlock(&G->lock);
  if (G->dbg)
    fprintf(stderr, "[gk] destroyed arena [%#lx,%#lx): %d memslots deleted, %ld tables in use\n",
            (unsigned long)base, (unsigned long)end, removed, G->pt_used);
  if (in_use > 0 || mmap((void *)base, end - base, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1,
                         0) == MAP_FAILED) {
    munmap((void *)base, end - base);
    if (in_use > 0) return;
    pthread_mutex_lock(&G->lock);  // unmapped instead: free, not held
    for (int i = 0; i < nslots; i++) G->slot_used[slot0 + i] = 0;
    pthread_mutex_unlock(&G->lock);
    return;
  }
  pthread_mutex_lock(&G->lock);
  for (int i = 0; i < nslots; i++) {
    G->slot_used[slot0 + i] = 0;
    G->slot_held[slot0 + i] = 1;
  }
  pthread_mutex_unlock(&G->lock);
}
