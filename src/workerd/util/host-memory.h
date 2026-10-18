// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/util/memory-class.h>

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/function.h>
#include <kj/string.h>

#include <cstddef>
#include <cstdint>
#include <new>

namespace workerd {

// Host memory: memory for runtime state that only host code touches, which no guest page-table
// root maps (memory class H in src/workerd/experimental/guest-kernel/memory-classes.md).
//
// With guest-kernel isolation compiled in and active (gk_init() has run, which workerd does
// under WORKERD_EXPERIMENTAL_GUEST_KERNEL before any isolate exists), the bytes come from
// gk_host_alloc(): a JS turn at guest ring 3, and any escape from one with a read or write
// primitive, cannot reach them -- a guest access faults at the accessed address rather than
// reading zeroes -- while host code reads and writes them as ordinary memory. Without the guest
// kernel (another platform, or the environment variable unset) the bytes come from the ordinary
// heap and everything here behaves the same, so code that uses host memory reads identically in
// both builds and is exercised by the ordinary test suite.
//
// Allocation is host-side only, and never on a path the guest kernel calls from a fault or
// syscall handler. It may throw (out of host memory); it never silently hands out ordinary
// memory when the guest kernel is active. Memory is zero-filled when allocated and scrubbed when
// freed.
namespace hostMemory {

// A zero-filled block of `size` bytes, aligned to at least `alignment` (at most 4 KiB).
void* allocate(size_t size, size_t alignment);

// Returns a block from allocate(), with the same `size` and `alignment`. The block is scrubbed
// first.
void deallocate(void* ptr, size_t size, size_t alignment);

// Whether `ptr` lies in guest-kernel host memory. False whenever the guest kernel is not active,
// including when `ptr` came from allocate(), since it is then ordinary heap memory.
bool contains(const void* ptr);

// A pool: host memory for a tenant whose footprint is configured rather than small -- a cache's
// keys, entries and values, bounded by an operator's limits -- which allocate() above is not
// sized for (the guest kernel serves it blocks of at most 1 MiB from a bounded total shared by
// every host-memory tenant in the process, so a configured store would starve the runtime's
// own records). A pool is reservation of its own: anonymous memory the pool maps itself, in
// regions, each registered with the guest kernel as refused (gk_host_range_register in
// experimental/guest-kernel/gk.h), after which a guest access anywhere in it faults as an
// access to any host memory does; without the guest kernel it is ordinary memory, and the pool
// behaves the same. Pages materialize as they are first touched, so the pool's footprint is
// its high-water mark and never the reservation.
//
// Blocks are served by power-of-two size class from 16 bytes up to a region, carved from a
// region's bump pointer and recycled through per-class free lists kept inside the freed blocks
// themselves, as the guest kernel's own allocator does: no splitting, no coalescing, internal
// fragmentation bounded by a factor of two, and address space reused through the lists. A
// block is scrubbed when freed and comes back zero-filled. allocate() returns nullptr, never
// throws, when the reservation is spent, so that a tenant on a path that cannot throw -- a
// hostcall handler -- refuses its operation instead; ensure() is where the reservation grows,
// host-side, when the tenant's limits do. The pool takes no lock of its own: the tenant
// serializes its use under the lock that guards the tenant. The pool is itself host memory
// (a member of a host object, say), since its free-list heads are pointers the host
// dereferences.
class WD_MEMORY_CLASS_HOST HostPool {
 public:
  // At most this much reservation per pool, over at most REGIONS regions: configured limits
  // beyond it are served up to it.
  static constexpr size_t MAX_RESERVATION = size_t{64} << 30;
  static constexpr size_t MIN_REGION = size_t{64} << 20;
  static constexpr size_t MIN_BLOCK = 16;
  static constexpr unsigned REGIONS = 16;

  HostPool() = default;
  ~HostPool() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(HostPool);

  // Grows the reservation to at least `capacity` bytes (clamped to MAX_RESERVATION) by mapping
  // and registering another region. False if that failed, after which the pool is as it was.
  bool ensure(size_t capacity);
  size_t reservation() const {
    return reserved;
  }

  void* allocate(size_t size);
  // `size` must be the size the block was allocated with.
  void deallocate(void* ptr, size_t size);
  bool contains(const void* ptr) const;

 private:
  static constexpr unsigned CLASSES = 40;  // 2^4 .. 2^43, past MAX_RESERVATION
  struct Region {
    uintptr_t base;
    uintptr_t end;
    uintptr_t bump;
    bool registered;
  };
  Region regions[REGIONS] = {};
  unsigned regionCount = 0;
  size_t reserved = 0;
  void* freeLists[CLASSES] = {};

  static unsigned classOf(size_t size);
  const Region* regionOf(uintptr_t p) const;
};

// Host slots: a block of host memory named by a handle rather than a pointer, so that the owner
// keeps nothing outside host memory that the host would dereference. The table that maps handles
// to blocks is the guest kernel's (gk_host_slot_alloc in experimental/guest-kernel/gk.h): it
// lives in gk's supervisor control block, beyond a guest's reach, has a fixed capacity of
// SLOT_CAPACITY entries, and validates a handle completely -- bounds, liveness and generation --
// before resolving it, reading nothing through the handle's value. Without the guest kernel a
// table of the same shape and the same checks backs the handles on the ordinary heap, so the
// handle semantics, including the refusal of a stale or foreign handle, are the same in both
// builds and are exercised by the ordinary test suite. The one semantic the fallback cannot
// have is the owner check below, which is a ring distinction: without the guest kernel there
// is no ring 3 to refuse.
//
// A handle is 0 only when it names nothing. allocateSlot() throws when the table or host memory
// is exhausted. resolveSlot() returns nullptr for any handle that does not name a live slot --
// a freed one, including after the slot is reused, or a value that never came from the table --
// and the guest kernel reports such a handle on stderr. freeSlot() refuses such a handle the
// same way and frees nothing. The pointer resolveSlot() returns is for immediate use; it is
// resolved again for each use rather than kept.
using SlotHandle = uint64_t;
constexpr size_t SLOT_CAPACITY = 4096;
SlotHandle allocateSlot(size_t size, size_t alignment);
void* resolveSlot(SlotHandle handle);
void freeSlot(SlotHandle handle);

// Resolve handles: a host object named by a handle that is resolved on every use against a table
// the guest cannot write, for the object an isolate-side holder (a JS heap object's C++ wrapper)
// reaches on nearly every API call. The table is the guest kernel's ring-0 resolve table
// (gk_resolve in experimental/guest-kernel/gk.h): it lives in supervisor memory, so a JS turn at
// guest ring 3 can neither read nor write an entry, and a turn resolves a handle through a trap
// into ring 0 inside the guest, with no VM exit, in about 19 ns; host code outside the guest
// reads the same table directly. A handle is validated completely before anything is returned --
// the entry must be live and its generation must be the handle's -- and nothing is ever read
// through the handle's own value, so a handle a turn has rewritten, to a pointer or to anything
// else, resolves to nothing rather than to an address the turn chose. Without the guest kernel a
// table of the same shape and the same checks backs the handles on the ordinary heap, so the
// handle semantics, including the refusal of a stale or foreign handle, are the same in both
// builds and are exercised by the ordinary test suite.
//
// The table is process-wide, with RESOLVE_CAPACITY entries for every isolate in the process
// together; registerResolve() returns 0 when it is full, which the caller turns into a failed
// operation. Registration and unregistration take a lock and are host-side; resolve() takes no
// lock and allocates nothing. The pointer resolve() returns is for immediate use, resolved
// again for each use rather than kept, and names an object the caller must still be entitled
// to: a handle rewritten to another live entry of the same isolate names that entry's object,
// which the table cannot tell from a legitimate use (the ownership residual the host slot
// table carries too). Another isolate's entry is refused, by the owner the entry records
// (below). unregisterResolve() refuses a handle that does not name a live entry, with a
// report, and retires nothing.
//
// A registration may carry RESOLVE_AUX_WORDS words of the registrant's own beside the pointer.
// Inside a turn they are journaled with it and handed, host-side, to the turn's committed hook
// (ResolveTurnHooks below) once the table has taken the pointer; outside a turn there is no
// hook, and the caller acts on them itself. The table never interprets them.
using ResolveHandle = uint64_t;
constexpr size_t RESOLVE_CAPACITY = size_t{1} << 20;
constexpr size_t RESOLVE_AUX_WORDS = 4;

// What the entry's pointer points at. The owner says whose entry it is and says nothing about
// what it is, so a handle rewritten to another live entry of the same isolate resolves and the
// caller casts the pointer to the class it expected: a turn that writes one of its own handles
// over another of its own gets an object of the wrong class, calls through the wrong vtable,
// and on a drop has the host free an object another holder still names. So an entry carries a
// tag, which the guest kernel keeps in the entry beside the owner and hands back with the
// pointer, and resolveOfType() refuses a pointer whose tag is not the one asked for.
//
// resolveTypeTag<T>() is the tag for a C++ type: a constant folded from T's spelling at compile
// time, so that the comparison is against an immediate in the runtime's text rather than a
// value in writable memory -- a turn can write the runtime's data but not its text, which is
// mapped without W and which the guest kernel will not let a forwarded mprotect change
// (gk_mapping_change_ok). It is derived from the name rather than from an address or a counter
// because neither is reliably one value per type across the shared objects workerd links, and a
// tag that differed between them would refuse a legitimate use. Two types whose spellings
// collide share a tag, which costs a check that would have caught the swap and never a false
// refusal; RESOLVE_TYPE_NONE is the tag of an entry registered without one, which a typed
// resolve refuses and an untyped one accepts.
//
// What this does not cover: a swap between two live entries of the *same* type, which no tag
// can tell apart, and a turn that reaches code execution inside the runtime, which skips the
// comparison as it would skip any the runtime makes -- unlike the owner, which is compared at
// ring 0. See the type word in experimental/guest-kernel/gk.h.
constexpr uint64_t RESOLVE_TYPE_NONE = 0;
namespace _ {
constexpr uint64_t fnv1a64(const char* s) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (; *s != '\0'; ++s) {
    hash ^= static_cast<unsigned char>(*s);
    hash *= 0x100000001b3ull;
  }
  return hash;
}
}  // namespace _
template <typename T>
constexpr uint64_t resolveTypeTag() {
  // __PRETTY_FUNCTION__ spells T out inside this template, so the hash is of T's name and is
  // the same wherever T is named, in this shared object or another.
  constexpr uint64_t tag = _::fnv1a64(__PRETTY_FUNCTION__);
  return tag == RESOLVE_TYPE_NONE ? 1 : tag;
}

ResolveHandle registerResolve(void* ptr, uint64_t type);
ResolveHandle registerResolve(void* ptr, uint64_t type, const uint64_t (&aux)[RESOLVE_AUX_WORDS]);

// Which isolate an entry belongs to. The guest kernel records, in the entry, the page-table
// root of the isolate it was registered under, and a turn that resolves a handle at guest
// ring 3 gets nothing unless the entry is its own isolate's: that is what keeps a turn from
// rewriting its handle to another isolate's and reaching that isolate's object (gk_resolve in
// experimental/guest-kernel/gk.h). registerResolve() takes the isolate whose arena the calling
// thread is in, which inside a turn, and anywhere under the isolate lock, is the right one.
//
// A host thread that registers an entry for an isolate it is not locked into has no arena, and
// an entry registered that way would resolve for no turn at all. Such a caller names the
// isolate instead: IsolateArena is that isolate's arena as an opaque pointer, which
// Worker::Isolate::getGuestArena() hands out, and registerResolveFor() registers the entry
// under it. A null arena is the base root, which is what registerResolve() would have used.
// Host-side only: inside a turn the entries come from the turn's reserved pool, which is the
// turn's isolate's already.
using IsolateArena = void*;
ResolveHandle registerResolveFor(IsolateArena arena, void* ptr, uint64_t type);

// An entry registered inside a turn is owned by the arena that turn ran under, which is the
// isolate whose turn it was. The runtime builds objects for one isolate inside turns of
// another -- the IoContext of the worker a subrequest is addressed to is built by the calling
// worker's turn -- and such an entry has to end up owned by the isolate whose turns will
// resolve it. Nothing a turn supplies can say which isolate that is, so neither of these takes
// an arena.
//
// disownResolve() leaves the entry owned by no arena, where it resolves for no turn at all;
// adoptResolve() gives it to the isolate whose arena the calling thread is in, taken from gk's
// record of the thread, and is how that isolate claims the entry host-side, on its own thread,
// under its own lock. Both identify the entry by what it holds: the handle must name a live
// entry whose pointer is exactly `ptr` and whose tag is `type`, so a caller passing its own
// object's address can re-own that entry and no other, whatever handle it was given. False,
// with a report, otherwise. Host-side only. Without the guest kernel there are no arenas and
// both do nothing.
bool disownResolve(ResolveHandle handle, void* ptr, uint64_t type);
bool adoptResolve(ResolveHandle handle, void* ptr, uint64_t type);
void* resolve(ResolveHandle handle);

// resolve(), for a caller that knows what the entry should be: null unless the entry's tag is
// `type` (see resolveTypeTag above). RESOLVE_TYPE_NONE asks for no comparison, which is what
// resolve() does.
void* resolveOfType(ResolveHandle handle, uint64_t type);
void unregisterResolve(ResolveHandle handle);

// Retires a live entry through the table directly, for host code that runs while a turn's
// journal is active on the thread -- a hostcall handler -- where unregisterResolve() would
// journal the release for the turn instead. Refuses a handle that names nothing, with a report.
void retireResolve(ResolveHandle handle);

// Entries registered or reserved and not yet unregistered, for tests and diagnostics.
size_t liveResolveEntries();

// Companions: one host-only record per live entry, in host memory, for what the host keeps
// about the object an entry names and the turn must never reach -- the owned-list node behind
// an IoOwn's entry (io/io-own.h), say, while the entry itself holds the object pointer the turn
// resolves. A companion is set and read host-side only, and is validated like a resolve: the
// handle must name a live committed entry, and the record is found by the entry's index in a
// table the host owns, never through the handle's value. The record's address is stable for as
// long as the entry is live, so host state may point into it. Unregistering the entry clears
// its companion. Without the guest kernel the table is on the ordinary heap, like the entries.
struct ResolveCompanion {
  uint64_t kind;  // the owner's tag for what `ptr` is; 0 when the entry has no companion
  void* ptr;
};
bool setResolveCompanion(ResolveHandle handle, ResolveCompanion companion);
kj::Maybe<ResolveCompanion&> resolveCompanion(ResolveHandle handle);

// Whether a turn is running on this thread, as the turn's own bookkeeping says. Outside a turn
// the answer is the host's, since the host resets that bookkeeping when the turn returns; inside
// one a turn that lies takes the host's path at ring 3 and faults on the memory it reaches.
bool inTurn();

// In a turn: visits, in registration order, the entries this turn has registered and not yet
// released, and releases those for which `fn` returns true -- as unregisterResolve() would, with
// nothing committed for them after the turn. For a holder that is torn down inside the turn
// that created some of its objects.
void releaseJournaled(
    kj::FunctionParam<bool(ResolveHandle handle, void* ptr, const uint64_t* aux)> fn);

// In a turn: retires, through one hostcall, the committed entries this turn has released so
// far, with their released hook run, as the end of the turn would. For a holder that is about
// to walk its host-side state through a hostcall and must not find what it has already released.
void flushReleasedInTurn();

// In a turn: records `words` for the host to act on once the turn has returned (through
// ResolveTurnHooks::noted), for an operation on host state that the turn cannot perform itself
// and that need not be done before the turn ends -- queuing a dropped holder's handle for the
// context that owns the object, say. The note is the turn's own memory until the host reads it,
// by position and bounded by the host's count, and what the words mean is for the hook to
// validate. A turn that outruns its notes hands them over through one hostcall. Returns false
// outside a turn, where the caller acts on the host state directly, and when the note could
// not be recorded.
constexpr size_t NOTE_WORDS = 4;
bool noteInTurn(const uint64_t (&words)[NOTE_WORDS]);

// Whether the words of the turn being finished on this thread may name `handle`. The journal
// is the turn's own memory, so every handle the host takes out of it -- the retire list, the
// notes, the context an owned object's registration names -- is a value the turn chose, and a
// handle that resolves is not thereby the turn's: the host's own lookup answers for whichever
// context it serves and makes no owner comparison (see gk_resolve in
// experimental/guest-kernel/gk.h). This is that comparison, against the arena the turn ran
// under, which the host recorded at the turn's entry from gk's own record of the thread. The
// ResolveTurnHooks call it for the handles their words carry; the retire list is checked for
// them.
//
// It answers the cross-isolate question and only that. A handle that names nothing -- a context
// that ended before the turn did, an entry released earlier in the same turn -- is allowed
// through, for the operation to refuse in its own terms, as it did before this check existed;
// what is refused here is a live entry of another isolate. Outside the processing of a journal
// it is true throughout: nothing came from a turn, so there is nothing to distrust, which is
// how a hook run for a registration the host makes itself (OwnedObjectList::add outside a turn)
// sees it. Without the guest kernel there are no arenas and it is true.
bool ownedByTurn(ResolveHandle handle);

// Whether `handle` names a live committed entry of the isolate whose arena the calling thread
// is in, by gk's record of the thread: the comparison ownedByTurn makes for a journal's words,
// for a hostcall handler handed a handle in its frame. A thread on no arena -- host code outside
// any isolate's lock -- is not a turn, and the answer there is whether the entry is live at all.
// Without the guest kernel there are no arenas and a live entry is anyone's.
bool ownedByCurrentArena(ResolveHandle handle);

// The arena of the calling thread by gk's record of it, as registerResolveFor() takes one: for
// a host-side record that must remember which isolate made it (a cache binding's record of its
// binder, say), and for a hostcall handler comparing that record against the thread it runs on.
// Null on a thread with no arena -- host code outside any isolate's lock -- and always without
// the guest kernel.
IsolateArena currentIsolateArena();

// Whether [addr, addr + len) lies within the arena of the calling thread, by gk's record of the
// thread (gk_arena_range_ok in experimental/guest-kernel/gk.h): for a hostcall handler handed a
// range the turn named for the host to write into or read from, which is admissible only into
// the caller's own arena. False on a thread with no arena. Without the guest kernel there are no
// arenas, the handler is a direct call from the caller's own frame, and the answer is true.
bool arenaRangeOk(const void* addr, size_t len);

// A hook the host runs at the end of every turn (see ResolveTurn), host-side, after the turn's
// journal has been processed: for work a hostcall handler found to do but may not do itself,
// such as waking another isolate's waiters, which allocates and may queue for another context.
// One per process, set once host-side before any turn runs; the hook must not make a hostcall
// and runs under the isolate lock of the turn that ended, so what it allocates for others goes
// under a SharedHeapScope (jsg/heap-arena.h).
void setTurnEndHook(void (*hook)());

// What the host does with the entries a turn registered and released and the notes it made.
// All run host-side, with the table already updated for the entry: committed() once the table
// has taken a registration's pointer, with the words the registration carried, and may refuse
// it, after which the entry is retired and resolves to nothing; released() for a committed
// entry the turn released, before the entry is retired; noted() for each note, after the
// commits and the releases, so that a note about an entry the turn registered finds it
// committed and one about an entry the turn released finds it gone.
//
// committed(), released() and noted() may also run inside a hostcall -- a turn that outruns
// its reserved entries, its retire list or its notes -- so they must not take the process heap
// and must not make a hostcall of their own; what they may take is host memory and the guest
// kernel's own locks. noted() is told when it runs there (`inHandler`) and returns whether it
// consumed the note, and one it could not consume there is handed to it again at the end of
// the turn, where it must.
//
// refused() runs for a registration the entry does not keep, and always at the end of the
// turn, where it may dispose of what the registration named: a disposer may do either of the
// things a handler must not, so a refusal a settle inside a hostcall found is held until the
// turn returns. The entry is retired when the refusal is found, not when the hook runs, so
// nothing resolves the handle in between, and a turn that outruns the refusals the host can
// hold is refused its next registration rather than losing one of them.
struct ResolveTurnHooks {
  bool (*committed)(ResolveHandle handle, void* ptr, const uint64_t* aux);
  void (*released)(ResolveHandle handle);
  bool (*noted)(const uint64_t* words, bool inHandler);
  // For a registration that does not survive the turn: the table would not take the pointer,
  // or committed() refused it. The entry is retired either way and no holder can reach what
  // the registration named, so this is where the owner gives the object up. It runs after
  // committed() for a refusal, with the same words.
  void (*refused)(ResolveHandle handle, void* ptr, const uint64_t* aux);
};

// A JS turn's registrations, batched. The table is written only by the host, and the objects
// an IoOwn names are created inside turns, at guest ring 3, where the table cannot be written
// and where an exit per object (a hostcall, microseconds) would cost more than the lookups
// save. So the host reserves entries for the thread before the turn and commits them after it:
// a ResolveTurn on the host frame around a turn (io/io-context.c++, runJsTurn) tops the thread's
// pool of reserved entries up, hands the turn a journal with their handles, and once the guest
// has returned commits each handle the turn used with the pointer the turn journaled for it,
// runs the hooks, retires the ones the turn released, and keeps the rest for the next turn.
// Inside the turn, registerResolve() takes the next reserved handle and journals the pointer
// and words, resolve() falls back to the journal for a handle the table does not yet hold, and
// unregisterResolve() marks a journaled entry released or lists a committed one for retirement
// at the end, and noteInTurn() records what the host is to do once the turn has returned; a
// turn that outruns its retire list or its notes flushes it through one hostcall. Nested in a
// turn, a ResolveTurn does nothing: the outermost frame commits.
//
// A turn that outruns its reserved entries refills through one hostcall too, but a refill is
// not a top-up: the positions the turn has spent each still hold an entry of the pool, so
// there is nothing to top up until they are settled. The handler therefore does there what
// the end of the turn does -- commits each spent position and runs the hooks for it -- and
// starts the journal again over fresh reservations. A turn pays one exit per pool's worth of
// registrations and is bounded by the table rather than by the pool, and a registration made
// before a refill is committed, and so resolvable through the table, from that refill on
// rather than from the end of the turn. What the table will not take, or the hook will not
// keep, is retired there and handed to refused() when the turn returns; a refill that has
// more of those than it can hold settles no further, which leaves the turn out of entries
// and its next registration refused.
//
// What the host trusts. The pool -- which handles it reserved for this thread, and the hooks --
// is in host memory and found through the guest kernel's per-thread words, never through
// anything a turn can write; the journal is ordinary memory the turn writes, and the host reads
// it by position, bounded by its own counts, taking from it only the pointers, which the table
// validates on commit (gk_resolve_commit: memory the entry's owner can reach, so outside gk's
// protected ranges and outside every arena but the committing thread's own), and the words,
// which the hook validates. A turn can therefore commit
// pointers only into the entries reserved for it, and only pointers to memory it could already
// write directly; it cannot commit into another tenant's entry, which is committed once, nor
// send the host into memory it cannot reach itself. A turn's lies about its own journal cost
// only that turn its registrations.
class ResolveTurn {
 public:
  explicit ResolveTurn(ResolveTurnHooks hooks);
  ~ResolveTurn() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ResolveTurn);

 private:
  void* pool;  // the thread's pool, as the host found it at entry; null when nested in a turn
};

// Installs the hostcall a turn uses to refill its reserved entries or flush its retire list.
// Called once, host-side, after gk_init() and before any turn runs; a no-op without the guest
// kernel, where the handler is called directly.
void installResolveHostcall();

// Test-only hooks for the guest kernel's end-to-end check (experimental/guest-kernel/
// host-memory-test.sh). A tenant registers itself only when WORKERD_GK_TEST_PROBE is in the
// environment; there is none otherwise.
//
// A probe target is the address of one host-memory tenant, registered under a name, which a
// request from inside a JS turn reads (see maybeForceGuestFaultForTest in api/global-scope.c++),
// expecting the guest fault a ring-3 read of host memory takes. "token-key" is the channel token
// key (server/channel-token.c++), "host-entropy" the host entropy buffer (host-entropy.c++) and
// "actor-id-key" the first actor-id namespace's HMAC key (server/actor-id-impl.c++).
void setProbeTargetForTest(kj::StringPtr name, const void* target);
kj::Maybe<const void*> probeTargetForTest(kj::StringPtr name);

// The handle probe is where the tenant's owner keeps the handle that names the tenant (the
// sizeof(SlotHandle) bytes a HostSlot occupies) together with a host-side use of the tenant
// through that handle, which logs whether the use resolved the tenant and at what address or was
// refused. A request to /__gk_corrupt_handle overwrites the handle from inside a JS turn with the
// address of a buffer the turn chose, as a write primitive would a pointer, then runs the use on
// the event loop: it must be refused rather than reach the buffer. The handle is restored and
// the use run again afterwards.
struct HandleProbeForTest {
  void* handleStorage;
  kj::Function<void()> useOnHost;
};
void setHandleProbeForTest(HandleProbeForTest probe);
kj::Maybe<HandleProbeForTest&> handleProbeForTest();

// How much the refusal paths may say. A refusal report -- a handle that names nothing, a
// registration that names another isolate's context, a journaled word of a kind the host does not
// know -- is the only evidence an operator has that something is forging what the host acts on.
// It is also the turn's to provoke: the words and handles a turn hands over are its own memory, so
// it can draw a report for every one of them, as fast as it can journal them, and the host formats
// and writes each. Unbounded, that is a denial of service on the log, on the disk under it, and on
// every other tenant's diagnostics in it.
//
// So each report site has a budget, and the counters live where the turn cannot reach them: gk's
// control block, which ring 3 can neither read nor write and which is found at a link-time address
// rather than through a pointer (gk_log_budget in experimental/guest-kernel/gk.h). Process memory
// would not do -- a turn that can zero a counter has no cap at all. The first report at a site is
// always made, whatever came before it, so the first evidence of an attack is never what gets
// dropped, and the report that is made carries the number held back since the last one, so a
// suppressed run shows as a number rather than as nothing.
//
// The sites, one per message, continue gk's own ids: gk keeps everything below GK_LOG_SITES_GK for
// its refusal paths. Adding a message means adding an id here.
constexpr unsigned LOG_SITE_BASE = 16;  // GK_LOG_SITES_GK
enum class LogSite : unsigned {
  SLOT_RESOLVE = LOG_SITE_BASE,
  SLOT_FREE,
  RESOLVE_COMMIT,
  RESOLVE_COMMIT_NULL,
  RESOLVE_UNREGISTER,
  COMPANION_DIRECTORY,
  RETIRE_FOREIGN,
  NOTE_UNCONSUMED,
  COMMIT_HOOK_REFUSED,
  COMMIT_FAILED,
  COMPANION_SET,
  QUEUE_DELETION_REGISTRY,
  QUEUE_DELETION_UNKNOWN,
  QUEUE_DELETION_FOREIGN,
  QUEUE_ACTION_REGISTRY,
  REGISTRATION_NO_DISPOSER,
  REGISTRATION_FOREIGN_CONTEXT,
  REGISTRATION_DEAD_CONTEXT,
  REGISTRATION_UNKNOWN_KIND,
  RECORD_RELEASED_OBJECTS,
  RECORD_RELEASED_QUEUE,
  NOTE_FOREIGN_HANDLE,
  NOTE_FOREIGN_CONTEXT,
  NOTE_UNKNOWN_KIND,
  IOPTR_WRONG_CLASS,
  DROP_WRONG_CLASS,
  RELEASE_WRONG_CLASS,
  RELEASE_UNKNOWN,
  TAKE_WRONG_CLASS,
  TAKE_UNKNOWN,
  LIST_NO_RECORD,
  HANDLER_TEARDOWN_OBJECTS,
  USE_UNKNOWN,
  DELETION_NOT_NOTED,
  ADOPT_FAILED,
  TEARDOWN_IN_TURN,
  TEARDOWN_REGISTRY,
  TEARDOWN_OBJECTS,
};

// Whether `site` may report now, and what the report would stand for: 0 for one to hold back, or
// 1 plus the number held back at this site since the last one that was made. Safe at ring 3,
// where the budget is out of reach and the answer comes from the host through the hostcall a turn
// already makes; the exit is the one the report's own write would have cost. Only a path that is
// refusing something calls it.
uint32_t logBudget(LogSite site);

// Whether the calling thread is running a hostcall handler, as the guest kernel's record of
// the thread says (gk_in_handler). A handler runs with host privilege at a moment the guest
// chose, which may be the middle of the allocator with its lock held, so it must not take the
// process heap -- and KJ_LOG formats, which does. A refusal report made under one is therefore
// composed where the refusal was found and written when the turn returns. False without the
// guest kernel, where a hostcall is a direct call on the host side with no guest stopped
// anywhere.
bool inHandler();

namespace _ {

// The most a deferred report's text runs to. What does not fit is dropped: the message and the
// first of its values are what name the refusal, and the alternative here is an allocation.
constexpr size_t REPORT_TEXT = 240;

// Appends as much of `chars` as `buf` has room for.
void appendReportChars(char* buf, size_t& n, kj::ArrayPtr<const char> chars);

// Takes the next argument name off `macroArgs` -- KJ_LOG's `"" #__VA_ARGS__`, split on
// top-level commas as kj splits it -- and appends the delimiter, and `name = ` for a name that
// needs one. A string literal stands on its own, as it does in a line KJ_LOG writes.
void appendReportLabel(char* buf, size_t& n, const char*& macroArgs, bool first);

template <typename T>
void appendReportArg(char* buf, size_t& n, const char*& macroArgs, bool& first, T&& value) {
  appendReportLabel(buf, n, macroArgs, first);
  first = false;
  auto chars = kj::toCharSequence(kj::fwd<T>(value));
  appendReportChars(buf, n, kj::arrayPtr(chars.begin(), chars.size()));
}

}  // namespace _

// Holds a report for the end of the turn, where it is written as the refusal would have
// written it. Takes the text rendered, so nothing it does allocates.
void deferReport(const char* file, int line, kj::ArrayPtr<const char> text, uint32_t suppressed);

// Renders a report the way KJ_LOG would and hands it to deferReport. The values are rendered
// here rather than kept, because what they name -- a handle, a pointer the turn supplied -- may
// name nothing by the time the turn ends; kj renders every type these reports carry without
// allocating, and the buffer is on the stack.
template <typename... Params>
void deferReportArgs(
    const char* file, int line, const char* macroArgs, uint32_t suppressed, Params&&... params) {
  char buf[_::REPORT_TEXT];
  size_t n = 0;
  bool first = true;
  (_::appendReportArg(buf, n, macroArgs, first, kj::fwd<Params>(params)), ...);
  deferReport(file, line, kj::arrayPtr(buf, n), suppressed);
}

// The words of the note the turn on this thread recorded last (noteInTurn), where it recorded
// them: the journal, which is the turn's own memory. A test rewrites a note through this from
// inside the turn that made it, as a write primitive would, for the check that the host
// validates what a note says rather than acting on it (/__gk_action_steal in
// api/global-scope.c++). Null outside a turn and when the turn has recorded no note.
uint64_t* lastNoteForTest();

// A host use is a host-side operation registered under a name, which a request runs on the
// event loop, outside the guest, after a turn has done something to the memory it draws on, and
// which logs what it got. "mint-token" (server/channel-token.c++) mints an RPC channel token and
// logs its nonce, for the check that the nonce does not come from the thread-local entropy
// buffer a turn can overwrite (see /__gk_poison_entropy in api/global-scope.c++).
void setHostUseForTest(kj::StringPtr name, kj::Function<void()> use);
kj::Maybe<kj::Function<void()>&> hostUseForTest(kj::StringPtr name);

}  // namespace hostMemory

// A refusal report, budgeted (see hostMemory::logBudget) and safe on a path a hostcall handler
// reaches (see hostMemory::inHandler). The line carries `suppressedSinceLast`, the number of
// reports this site held back since the last one it made, so a run that was mostly held back is
// still visible as a number. Under a handler the line is rendered on the spot and written when
// the turn returns: the same line, from the same place, a moment later.
#define WD_LOG_REFUSAL(site, ...)                                                                  \
  do {                                                                                             \
    uint32_t wdRefusalBudget = ::workerd::hostMemory::logBudget(site);                             \
    if (wdRefusalBudget != 0) {                                                                    \
      uint32_t suppressedSinceLast KJ_UNUSED = wdRefusalBudget - 1;                                \
      if (::workerd::hostMemory::inHandler()) {                                                    \
        ::workerd::hostMemory::deferReportArgs(                                                    \
            __FILE__, __LINE__, "" #__VA_ARGS__, suppressedSinceLast, __VA_ARGS__);                \
      } else {                                                                                     \
        KJ_LOG(ERROR, __VA_ARGS__, suppressedSinceLast);                                           \
      }                                                                                            \
    }                                                                                              \
  } while (false)

// A T constructed in host memory, owned like a kj::Own: created by hostOwned<T>(...), destroyed
// (and its block scrubbed and freed) when the HostOwn goes away. Move-only.
//
// What this protects is the object's bytes: with the guest kernel active no guest access reaches
// them. The pointer to the object lives wherever the HostOwn itself lives, and the host
// dereferences it, so by the provenance rule in memory-classes.md the HostOwn belongs in a Host
// object too -- a HostOwn held in ordinary heap memory keeps the guest from reading the object,
// but not from swapping the pointer for one of its choosing. Freeing a swapped pointer is
// refused by the guest kernel as a fatal integrity error rather than written through.
template <typename T>
class WD_MEMORY_CLASS_HOST HostOwn {
 public:
  HostOwn(): ptr(nullptr) {}
  HostOwn(decltype(nullptr)): ptr(nullptr) {}
  HostOwn(HostOwn&& other) noexcept: ptr(other.ptr) {
    other.ptr = nullptr;
  }
  HostOwn& operator=(HostOwn&& other) noexcept {
    if (this != &other) {
      reset();
      ptr = other.ptr;
      other.ptr = nullptr;
    }
    return *this;
  }
  ~HostOwn() noexcept(false) {
    reset();
  }
  KJ_DISALLOW_COPY(HostOwn);

  T* get() {
    return ptr;
  }
  const T* get() const {
    return ptr;
  }
  T& operator*() {
    return *checked();
  }
  const T& operator*() const {
    return *checked();
  }
  T* operator->() {
    return checked();
  }
  const T* operator->() const {
    return checked();
  }
  bool operator==(decltype(nullptr)) const {
    return ptr == nullptr;
  }

 private:
  T* ptr;

  explicit HostOwn(T* ptr): ptr(ptr) {}

  T* checked() const {
    KJ_REQUIRE(ptr != nullptr, "HostOwn is empty");
    return ptr;
  }

  void reset() {
    if (ptr != nullptr) {
      T* p = ptr;
      ptr = nullptr;
      p->~T();
      hostMemory::deallocate(p, sizeof(T), alignof(T));
    }
  }

  template <typename U, typename... Params>
  friend HostOwn<U> hostOwned(Params&&... params);
};

// Constructs a T in host memory, as kj::heap<T>(...) would on the heap.
template <typename T, typename... Params>
HostOwn<T> hostOwned(Params&&... params) {
  static_assert(alignof(T) <= 4096, "host memory blocks are aligned to at most 4 KiB");
  void* mem = hostMemory::allocate(sizeof(T), alignof(T));
  bool constructed = false;
  KJ_DEFER({
    if (!constructed) hostMemory::deallocate(mem, sizeof(T), alignof(T));
  });
  T* obj = new (mem) T(kj::fwd<Params>(params)...);
  constructed = true;
  return HostOwn<T>(obj);
}

// A T constructed in host memory and owned through a host slot handle (hostMemory::allocateSlot)
// rather than a pointer: created by hostSlot<T>(...), destroyed (and its block scrubbed and
// freed) when the HostSlot goes away. Move-only. This is the form a host-memory tenant takes when
// its owner is not itself in host memory: the handle is the HostSlot's only member, so the owner
// holds nothing the host dereferences, and every access resolves the handle through the guest
// kernel's table, which checks it against the table alone. A handle the guest has rewritten
// resolves to nothing, and an access through it throws rather than reading or writing wherever
// the rewritten value pointed; the destructor then frees nothing. What remains is the ownership
// bound the table cannot close: a handle rewritten to another live tenant's handle names that
// tenant's object. The HostOwn above is the cheaper choice for an owner that is itself in host
// memory, where the pointer is as unreachable as the object.
template <typename T>
class WD_MEMORY_CLASS_HOST HostSlot {
 public:
  HostSlot(): handle(0) {}
  HostSlot(decltype(nullptr)): handle(0) {}
  HostSlot(HostSlot&& other) noexcept: handle(other.handle) {
    other.handle = 0;
  }
  HostSlot& operator=(HostSlot&& other) noexcept {
    if (this != &other) {
      reset();
      handle = other.handle;
      other.handle = 0;
    }
    return *this;
  }
  ~HostSlot() noexcept(false) {
    reset();
  }
  KJ_DISALLOW_COPY(HostSlot);

  // The object, or nullptr when the HostSlot is empty or its handle no longer names a live slot.
  T* get() {
    return resolve();
  }
  const T* get() const {
    return resolve();
  }
  T& operator*() {
    return *checked();
  }
  const T& operator*() const {
    return *checked();
  }
  T* operator->() {
    return checked();
  }
  const T* operator->() const {
    return checked();
  }
  bool operator==(decltype(nullptr)) const {
    return handle == 0;
  }

  // The handle itself, for a hostcall (util/hostcall.h) whose handler resolves it host-side: the
  // handle is the one thing about the object that may cross the boundary, since the handler
  // validates it against the table like any other use. 0 when the HostSlot is empty.
  hostMemory::SlotHandle getHandle() const {
    return handle;
  }

 private:
  hostMemory::SlotHandle handle;

  explicit HostSlot(hostMemory::SlotHandle handle): handle(handle) {}

  T* resolve() const {
    if (handle == 0) return nullptr;
    return static_cast<T*>(hostMemory::resolveSlot(handle));
  }

  T* checked() const {
    KJ_REQUIRE(handle != 0, "HostSlot is empty");
    T* obj = static_cast<T*>(hostMemory::resolveSlot(handle));
    if (obj == nullptr) {
      KJ_FAIL_REQUIRE("HostSlot handle does not name a live host slot", kj::hex(handle));
    }
    return obj;
  }

  void reset() {
    if (handle != 0) {
      hostMemory::SlotHandle h = handle;
      handle = 0;
      // A handle that no longer resolves names nothing of ours: the destructor is not run and
      // freeSlot() refuses the handle, so nothing is written through it.
      T* obj = static_cast<T*>(hostMemory::resolveSlot(h));
      if (obj != nullptr) obj->~T();
      hostMemory::freeSlot(h);
    }
  }

  template <typename U, typename... Params>
  friend HostSlot<U> hostSlot(Params&&... params);
};
static_assert(sizeof(HostSlot<int>) == sizeof(hostMemory::SlotHandle),
    "a HostSlot holds the handle and nothing else");

// Constructs a T in host memory owned through a host slot, as hostOwned<T>(...) does through a
// pointer.
template <typename T, typename... Params>
HostSlot<T> hostSlot(Params&&... params) {
  static_assert(alignof(T) <= 4096, "host memory blocks are aligned to at most 4 KiB");
  hostMemory::SlotHandle handle = hostMemory::allocateSlot(sizeof(T), alignof(T));
  bool constructed = false;
  KJ_DEFER({
    if (!constructed) hostMemory::freeSlot(handle);
  });
  void* mem = hostMemory::resolveSlot(handle);
  KJ_ASSERT(mem != nullptr, "a fresh host slot handle must resolve");
  new (mem) T(kj::fwd<Params>(params)...);
  constructed = true;
  return HostSlot<T>(handle);
}

}  // namespace workerd
