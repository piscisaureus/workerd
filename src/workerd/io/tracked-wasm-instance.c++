// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "tracked-wasm-instance.h"

#include <workerd/jsg/jsg.h>

#include <v8-array-buffer.h>
#include <v8-wasm.h>

namespace workerd {

kj::Maybe<TrackedWasmInstance&> TrackedWasmInstanceList::registerSignal(jsg::Lock&,
    kj::Array<kj::byte> memory,
    kj::Maybe<uint32_t> signalOffset,
    kj::Maybe<uint32_t> terminatedOffset) const {
  // At least one offset must be provided — there's nothing to register otherwise.
  if (signalOffset == kj::none && terminatedOffset == kj::none) {
    return kj::none;
  }

  // If a terminated offset was provided, validate it fits in memory.
  KJ_IF_SOME(offset, terminatedOffset) {
    if (static_cast<size_t>(offset) + WASM_SIGNAL_FIELD_BYTES > memory.size()) {
      return kj::none;
    }
  }
  // If a signal offset was provided, validate it fits in memory too.
  KJ_IF_SOME(offset, signalOffset) {
    if (static_cast<size_t>(offset) + WASM_SIGNAL_FIELD_BYTES > memory.size()) {
      return kj::none;
    }
    // Zero the signal address to clear any stale signals.
    uint32_t value = 0;
    memory.asPtr().slice(offset, offset + WASM_SIGNAL_FIELD_BYTES).copyFrom(kj::asBytes(&value, 1));
  }

  // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock, which is the
  // synchronization required by the signal-safe list for mutations.
  auto& entry = const_cast<SignalSafeList<TrackedWasmInstance>&>(list).pushFront(
      TrackedWasmInstance{.memory = kj::mv(memory),
        .signalByteOffset = signalOffset,
        .terminatedByteOffset = terminatedOffset});
  return entry;
}

void TrackedWasmInstanceList::filter(jsg::Lock&) const {
  // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list).filter(
      [](const TrackedWasmInstance& signal) { return signal.shouldRetain(); });
}

void TrackedWasmInstanceList::clear(jsg::Lock&) const {
  // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list).clear();
}

void TrackedWasmInstanceList::refresh(jsg::Lock& js) const {
  js.withinHandleScope([&] {
    // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock.
    refreshWith(const_cast<SignalSafeList<TrackedWasmInstance>&>(list),
        [&](TrackedWasmInstance& entry) -> kj::Maybe<kj::Array<kj::byte>> {
      // An entry whose instance has been collected is about to be removed by filter().
      if (!entry.shouldRetain() || entry.memoryRef.IsEmpty()) return kj::none;
      auto backingStore = entry.memoryRef.Get(js.v8Isolate)->Buffer()->GetBackingStore();
      if (backingStore->Data() == entry.memory.begin()) return kj::none;
      return kj::arrayPtr(static_cast<kj::byte*>(backingStore->Data()), backingStore->ByteLength())
          .attach(kj::mv(backingStore));
    });
  });
}

void TrackedWasmInstanceList::refreshWith(
    SignalSafeList<TrackedWasmInstance>& list, CurrentMemory currentMemory) {
  // A replacement goes to the front of the list, ahead of the node this traversal started from,
  // so it is not visited again. The replaced node keeps its (stale but still allocated) memory
  // until filter() removes it, so a signal handler that interrupts this at any point only ever
  // sees complete entries with live memory.
  list.iterate([&](TrackedWasmInstance& entry) {
    KJ_IF_SOME(memory, currentMemory(entry)) {
      // Linear memory never shrinks, so offsets validated at registration still fit. Check
      // anyway rather than ever let a signal handler write out of bounds.
      auto fits = [&](kj::Maybe<uint32_t> offset) {
        KJ_IF_SOME(o, offset) {
          return static_cast<size_t>(o) + WASM_SIGNAL_FIELD_BYTES <= memory.size();
        }
        return true;
      };
      if (!fits(entry.signalByteOffset) || !fits(entry.terminatedByteOffset)) return;

      list.pushFront(TrackedWasmInstance{
        .memory = kj::mv(memory),
        .signalByteOffset = entry.signalByteOffset,
        .terminatedByteOffset = entry.terminatedByteOffset,
        .instanceRef = kj::mv(entry.instanceRef),
        .memoryRef = kj::mv(entry.memoryRef),
      });
    }
  });
}

void TrackedWasmInstanceList::writeShutdownSignal() const {
  // Safe to const_cast: this is called from a signal handler on the same thread that holds the
  // isolate lock, so there is no concurrent mutation of the list structure.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list).iterate([](TrackedWasmInstance& signal) {
    KJ_IF_SOME(offset, signal.signalByteOffset) {
      uint32_t value = WASM_SIGNAL_SIGXCPU;
      signal.memory.asPtr().slice(offset, offset + sizeof(value)).copyFrom(kj::asBytes(&value, 1));
    }
  });
}

void TrackedWasmInstanceList::clearShutdownSignal() const {
  // Safe to const_cast: same-thread signal-handler context, no concurrent list mutation.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list).iterate([](TrackedWasmInstance& signal) {
    KJ_IF_SOME(offset, signal.signalByteOffset) {
      uint32_t value = 0;
      signal.memory.asPtr().slice(offset, offset + sizeof(value)).copyFrom(kj::asBytes(&value, 1));
    }
  });
}

void TrackedWasmInstanceList::writeTerminatedSignal() const {
  // Safe to const_cast: same-thread signal-handler context, no concurrent list mutation.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list).iterate([](TrackedWasmInstance& signal) {
    // Skip entries that have no terminated address (signal-only modules).
    KJ_IF_SOME(offset, signal.terminatedByteOffset) {
      uint32_t value = 1;
      signal.memory.asPtr().slice(offset, offset + sizeof(value)).copyFrom(kj::asBytes(&value, 1));
    }
  });
}

}  // namespace workerd
