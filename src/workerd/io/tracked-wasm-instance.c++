// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "tracked-wasm-instance.h"

#include <workerd/jsg/setup.h>

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#endif

#include <kj/debug.h>

#include <cerrno>
#include <cstring>

namespace workerd {

namespace {

// Whether the signal writers use the guest kernel's table. Read in a signal handler, from gk's
// supervisor memory, which no turn can write.
bool fieldsInGuestKernel() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  return gk_initialized() != 0;
#else
  return false;
#endif
}

void writeField(kj::Array<kj::byte>& memory, uint32_t offset, uint32_t value) {
  memory.asPtr().slice(offset, offset + sizeof(value)).copyFrom(kj::asBytes(&value, 1));
}

}  // namespace

kj::Maybe<TrackedWasmInstance&> TrackedWasmInstanceList::registerSignal(jsg::Lock& js,
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
    writeField(memory, offset, 0);
  }

  uint64_t signalHandle = 0;
  uint64_t terminatedHandle = 0;
  KJ_IF_SOME(arena, jsg::IsolateBase::from(js.v8Isolate).getGuestArena()) {
    // The guest kernel is on: the linear memory is in the isolate's arena (V8 places the
    // sandbox there), so it is described to the host by its offset within the arena. The
    // bounds are a copy in the heap and the host validates the result against its own record,
    // so a memory that lies outside the arena is refused there, not here; this check only
    // keeps the offset arithmetic meaningful.
    auto base = reinterpret_cast<uintptr_t>(arena.base());
    auto begin = reinterpret_cast<uintptr_t>(memory.begin());
    if (begin < base || begin - base > arena.size() ||
        memory.size() > arena.size() - (begin - base)) {
      KJ_LOG(WARNING,
          "guest-kernel: a Wasm linear memory lies outside the isolate's arena; the "
          "module will not be signaled");
      return kj::none;
    }
    if (!registerFields(begin - base, memory.size(), signalOffset, terminatedOffset, signalHandle,
            terminatedHandle)) {
      return kj::none;
    }
  }

  // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock, which is the
  // synchronization required by the signal-safe list for mutations.
  auto& entry = const_cast<SignalSafeList<TrackedWasmInstance>&>(list).pushFront(
      TrackedWasmInstance{.memory = kj::mv(memory),
        .signalByteOffset = signalOffset,
        .terminatedByteOffset = terminatedOffset,
        .signalFieldHandle = signalHandle,
        .terminatedFieldHandle = terminatedHandle});
  return entry;
}

bool TrackedWasmInstanceList::registerFields(uintptr_t memoryOffset,
    size_t memoryLength,
    kj::Maybe<uint32_t> signalOffset,
    kj::Maybe<uint32_t> terminatedOffset,
    uint64_t& signalHandle,
    uint64_t& terminatedHandle) {
  hostcall::Frame frame{};
  frame.args[0] = static_cast<uint64_t>(Op::REGISTER);
  frame.args[1] = memoryOffset;
  frame.args[2] = memoryLength;
  frame.args[3] = static_cast<uint64_t>(signalOffset.orDefault(NO_FIELD)) |
      (static_cast<uint64_t>(terminatedOffset.orDefault(NO_FIELD)) << 32);
  long result = hostcall::call(HOSTCALL, frame);
  uint64_t handles[2] = {0, 0};
  if (frame.outLen == sizeof(handles)) memcpy(handles, frame.buf, sizeof(handles));
  if (result != 0 || (signalOffset != kj::none) != (handles[0] != 0) ||
      (terminatedOffset != kj::none) != (handles[1] != 0)) {
    KJ_LOG(WARNING,
        "guest-kernel: the Wasm signal fields were refused; the module will not be signaled",
        result, memoryOffset, memoryLength, signalOffset, terminatedOffset);
    return false;
  }
  signalHandle = handles[0];
  terminatedHandle = handles[1];
  return true;
}

void TrackedWasmInstanceList::unregisterFields(TrackedWasmInstance& entry) {
  if (entry.signalFieldHandle == 0 && entry.terminatedFieldHandle == 0) return;
  hostcall::Frame frame{};
  frame.args[0] = static_cast<uint64_t>(Op::UNREGISTER);
  frame.args[1] = entry.signalFieldHandle;
  frame.args[2] = entry.terminatedFieldHandle;
  long result = hostcall::call(HOSTCALL, frame);
  if (result != 0) {
    KJ_LOG(ERROR, "guest-kernel: unregistering a module's Wasm signal fields failed", result,
        entry.signalFieldHandle, entry.terminatedFieldHandle);
  }
  entry.signalFieldHandle = 0;
  entry.terminatedFieldHandle = 0;
}

void TrackedWasmInstanceList::filter(jsg::Lock&) const {
  // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list).filter([](TrackedWasmInstance& entry) {
    if (entry.shouldRetain()) return true;
    unregisterFields(entry);
    return false;
  });
}

void TrackedWasmInstanceList::clear(jsg::Lock&) const {
  // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock.
  auto& mutableList = const_cast<SignalSafeList<TrackedWasmInstance>&>(list);
  mutableList.iterate([](TrackedWasmInstance& entry) { unregisterFields(entry); });
  mutableList.clear();
}

void TrackedWasmInstanceList::writeShutdownSignal() const {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (fieldsInGuestKernel()) {
    gk_signal_field_write(static_cast<unsigned>(FieldKind::SIGNAL), WASM_SIGNAL_SIGXCPU);
    return;
  }
#endif
  // Safe to const_cast: this is called from a signal handler on the same thread that holds the
  // isolate lock, so there is no concurrent mutation of the list structure.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list).iterate([](TrackedWasmInstance& signal) {
    KJ_IF_SOME(offset, signal.signalByteOffset) {
      writeField(signal.memory, offset, WASM_SIGNAL_SIGXCPU);
    }
  });
}

void TrackedWasmInstanceList::clearShutdownSignal() const {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (fieldsInGuestKernel()) {
    gk_signal_field_write(static_cast<unsigned>(FieldKind::SIGNAL), 0);
    return;
  }
#endif
  // Safe to const_cast: same-thread signal-handler context, no concurrent list mutation.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list).iterate([](TrackedWasmInstance& signal) {
    KJ_IF_SOME(offset, signal.signalByteOffset) {
      writeField(signal.memory, offset, 0);
    }
  });
}

void TrackedWasmInstanceList::writeTerminatedSignal() const {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (fieldsInGuestKernel()) {
    gk_signal_field_write(static_cast<unsigned>(FieldKind::TERMINATED), 1);
    return;
  }
#endif
  // Safe to const_cast: same-thread signal-handler context, no concurrent list mutation.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list).iterate([](TrackedWasmInstance& signal) {
    // Skip entries that have no terminated address (signal-only modules).
    KJ_IF_SOME(offset, signal.terminatedByteOffset) {
      writeField(signal.memory, offset, 1);
    }
  });
}

// The handler runs host-side at the turn's request, with the turn's arguments: what it passes
// to the guest kernel is validated there against gk's record of the calling thread's arena, so
// the handler itself checks only the operation. It allocates nothing; the guest kernel reports
// a handle it refuses on its own stderr path, which a turn can provoke by naming a field that
// is not its own.
long TrackedWasmInstanceList::hostcallHandler(hostcall::Frame& frame) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  switch (static_cast<Op>(frame.args[0])) {
    case Op::REGISTER: {
      uint32_t offsets[2] = {
        static_cast<uint32_t>(frame.args[3]), static_cast<uint32_t>(frame.args[3] >> 32)};
      constexpr unsigned KINDS[2] = {
        static_cast<unsigned>(FieldKind::SIGNAL), static_cast<unsigned>(FieldKind::TERMINATED)};
      gk_signal_handle handles[2] = {0, 0};
      if (offsets[0] == NO_FIELD && offsets[1] == NO_FIELD) return -EINVAL;
      for (int i = 0; i < 2; i++) {
        if (offsets[i] == NO_FIELD) continue;
        int result = gk_signal_field_register(static_cast<uintptr_t>(frame.args[1]),
            static_cast<size_t>(frame.args[2]), offsets[i], KINDS[i], &handles[i]);
        if (result != 0) {
          // Either both or neither: a first field registered before the second was refused is
          // taken back.
          if (i == 1 && handles[0] != 0) gk_signal_field_unregister(handles[0]);
          return result;
        }
      }
      static_assert(sizeof(handles) <= hostcall::BYTES);
      memcpy(frame.buf, handles, sizeof(handles));
      frame.outLen = sizeof(handles);
      return 0;
    }
    case Op::UNREGISTER: {
      long result = 0;
      for (int i = 1; i <= 2; i++) {
        if (frame.args[i] == 0) continue;
        int r = gk_signal_field_unregister(static_cast<gk_signal_handle>(frame.args[i]));
        if (r != 0) result = r;
      }
      return result;
    }
  }
#endif
  return -EINVAL;
}

const hostcall::Entry TrackedWasmInstanceList::HOSTCALL = {
  hostcall::WASM_SIGNAL, &TrackedWasmInstanceList::hostcallHandler};

void TrackedWasmInstanceList::installHostcall() {
  hostcall::install(HOSTCALL);
}

}  // namespace workerd
