#include <workerd/jsg/exception.h>
#include <workerd/server/actor-id-impl.h>
#include <workerd/util/host-entropy.h>
#include <workerd/util/own-util.h>
#include <workerd/util/thread-scopes.h>

#include <openssl/hmac.h>
#include <openssl/mem.h>

#include <kj/encoding.h>
#include <kj/memory.h>

#include <cerrno>
#include <cstdlib>

namespace workerd::server {

ActorIdFactoryImpl::ActorIdImpl::ActorIdImpl(
    const kj::byte idParam[SHA256_DIGEST_LENGTH], kj::Maybe<kj::String> name)
    : name(kj::mv(name)) {
  memcpy(id, idParam, sizeof(id));
}

kj::String ActorIdFactoryImpl::ActorIdImpl::toString() const {
  return kj::encodeHex(kj::ArrayPtr<const kj::byte>(id));
}

kj::Maybe<kj::StringPtr> ActorIdFactoryImpl::ActorIdImpl::getName() const {
  return name;
}

kj::Maybe<kj::StringPtr> ActorIdFactoryImpl::ActorIdImpl::getJurisdiction() const {
  return kj::none;
}

bool ActorIdFactoryImpl::ActorIdImpl::equals(const ActorId& other) const {
  return kj::arrayPtr(id) == kj::arrayPtr(kj::downcast<const ActorIdImpl>(other).id);
}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::ActorIdImpl::clone() const {
  return kj::heap<ActorIdImpl>(id, mapCopyString(name));
}

ActorIdFactoryImpl::ActorIdFactoryImpl(kj::StringPtr uniqueKey)
    : key(kj::rc<SharedKey>(hostSlot<Key>())) {
  // Host-side: a factory is constructed when a namespace is configured or a worker's bindings
  // are compiled, outside any turn, so the key can be written here.
  Key& k = *key->slot;
  k.tag = KEY_TAG;
  KJ_ASSERT(SHA256(uniqueKey.asBytes().begin(), uniqueKey.size(), k.bytes) == k.bytes);
  if (getenv("WORKERD_GK_TEST_PROBE") != nullptr &&
      hostMemory::probeTargetForTest("actor-id-key") == kj::none) {
    // Test-only (see hostMemory::setProbeTargetForTest): publish where the first namespace's
    // key lives, so the guest kernel's end-to-end check can read it from inside a JS turn and
    // expect the fault.
    const void* bytes = k.bytes;
    KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: actor-id key",
        kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(bytes))), hostMemory::contains(bytes));
    hostMemory::setProbeTargetForTest("actor-id-key", bytes);
  }
}

ActorIdFactoryImpl::ActorIdFactoryImpl(kj::Rc<SharedKey> key): key(kj::mv(key)) {}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::newUniqueId(
    kj::Maybe<kj::StringPtr> jurisdiction) {
  JSG_REQUIRE(
      jurisdiction == kj::none, Error, "Jurisdiction restrictions are not implemented in workerd.");

  // The first BASE_LENGTH bytes of the id are random, drawn host-side by the handler, and the
  // rest is their HMAC; in predictable mode the first half is a counter instead.
  kj::byte id[SHA256_DIGEST_LENGTH]{};
  hostcall::Frame frame{};
  if (isPredictableModeForTest()) {
    kj::byte base[BASE_LENGTH]{};
    memcpy(base, &counter, sizeof(counter));
    ++counter;
    keyedOp(frame, Op::MAC, base, kj::arrayPtr(id));
  } else {
    keyedOp(frame, Op::NEW_UNIQUE, nullptr, kj::arrayPtr(id));
  }
  return kj::heap<ActorIdImpl>(id, kj::none);
}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::idFromName(kj::String name) {
  // The first half of the id is the HMAC of the name (HMAC as a keyed hash, not for
  // authentication), the second half the HMAC of the first; both computed by the handler. A
  // name longer than one frame crosses in chunks, with the running hash kept host-side.
  kj::byte id[SHA256_DIGEST_LENGTH]{};
  hostcall::Frame frame{};
  auto bytes = name.asBytes();
  if (bytes.size() <= hostcall::BYTES) {
    keyedOp(frame, Op::NAME, bytes, kj::arrayPtr(id));
  } else {
    keyedOp(frame, Op::NAME_BEGIN, bytes.first(hostcall::BYTES), kj::none);
    bytes = bytes.slice(hostcall::BYTES);
    while (bytes.size() > hostcall::BYTES) {
      keyedOp(frame, Op::NAME_MORE, bytes.first(hostcall::BYTES), kj::none);
      bytes = bytes.slice(hostcall::BYTES);
    }
    keyedOp(frame, Op::NAME_END, bytes, kj::arrayPtr(id));
  }
  return kj::heap<ActorIdImpl>(id, kj::mv(name));
}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::idFromString(kj::String str) {
  return idFromStringNamed(kj::mv(str), kj::none);
}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::idFromStringNamed(
    kj::String str, kj::Maybe<kj::String> name) {
  auto decoded = kj::decodeHex(str);
  JSG_REQUIRE(str.size() == SHA256_DIGEST_LENGTH * 2 && !decoded.hadErrors &&
          decoded.size() == SHA256_DIGEST_LENGTH,
      TypeError, "Invalid Durable Object ID: must be 64 hex digits");
  return idFromRaw(decoded, kj::mv(name));
}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::idFromRaw(
    kj::ArrayPtr<const byte> bytes, kj::Maybe<kj::String> name) {
  KJ_REQUIRE(bytes.size() == SHA256_DIGEST_LENGTH, "Invalid Durable Object ID: must be 32 bytes");

  // Derive the id again from its first half and require the second half to match.
  kj::byte id[SHA256_DIGEST_LENGTH]{};
  hostcall::Frame frame{};
  keyedOp(frame, Op::MAC, bytes.first(BASE_LENGTH), kj::arrayPtr(id));
  JSG_REQUIRE(kj::arrayPtr(id).slice(BASE_LENGTH) == bytes.slice(BASE_LENGTH), TypeError,
      "Durable Object ID is not valid for this namespace.");

  return kj::heap<ActorIdImpl>(id, kj::mv(name));
}

kj::Own<ActorIdFactory> ActorIdFactoryImpl::cloneWithJurisdiction(
    kj::Maybe<kj::StringPtr> maybeJurisdiction) {
  if (maybeJurisdiction == kj::none) {
    return kj::heap<ActorIdFactoryImpl>(key.addRef());
  }

  JSG_FAIL_REQUIRE(Error, "Jurisdiction restrictions are not implemented in workerd.");
}

bool ActorIdFactoryImpl::matchesJurisdiction(const ActorId& id) {
  return true;
}

void ActorIdFactoryImpl::keyedOp(
    hostcall::Frame& frame, Op op, kj::ArrayPtr<const byte> in, kj::Maybe<kj::ArrayPtr<byte>> id) {
  KJ_REQUIRE(in.size() <= hostcall::BYTES);
  frame.args[0] = static_cast<uint64_t>(op);
  frame.args[1] = key->slot.getHandle();
  frame.args[2] = frame.args[3] = 0;
  frame.inLen = in.size();
  memcpy(frame.buf, in.begin(), in.size());
  long result = hostcall::call(HOSTCALL, frame);
  KJ_REQUIRE(result == 0, "actor-id hostcall failed", static_cast<uint64_t>(op), result);
  KJ_IF_SOME(out, id) {
    KJ_REQUIRE(frame.outLen == SHA256_DIGEST_LENGTH, "actor-id hostcall returned no id",
        static_cast<uint64_t>(op), frame.outLen);
    memcpy(out.begin(), frame.buf, SHA256_DIGEST_LENGTH);
  } else {
    KJ_REQUIRE(frame.outLen == 0);
  }
}

// =======================================================================================
// The host side: the handler. It runs on the calling thread, host-side, and takes everything
// it uses from the frame the guest kernel snapshotted, validating each field: the operation is
// one of the closed set, the handle resolves through the slot table to a block carrying
// KEY_TAG, the input length fits the operation, and a continuation finds the state its own
// predecessor left. It returns rather than throws, and allocates nothing on its success path:
// BoringSSL's HMAC keeps its digest contexts inline, and getHostEntropy() takes only its own
// lock in host memory.

namespace {

constexpr uint64_t NAME_TAG = 0x656d616e2d64692dull;

// The running HMAC of a name that crosses in chunks, kept in the frame's host-side state
// between calls on the thread. Tagged, and bound to the handle it was begun with, since the
// turn decides what calls come between.
struct NameState {
  uint64_t tag;
  uint64_t handle;
  HMAC_CTX ctx;
};
static_assert(sizeof(NameState) <= hostcall::STATE, "the name state must fit the frame's state");

// Fills `id` with `base` followed by the first half of HMAC(key, base); `base` and `id` must
// not overlap. False if the HMAC failed, which the one-shot HMAC over SHA-256 does not do.
bool deriveId(const kj::byte key[SHA256_DIGEST_LENGTH],
    const kj::byte base[SHA256_DIGEST_LENGTH / 2],
    kj::byte id[SHA256_DIGEST_LENGTH]) {
  constexpr size_t BASE = SHA256_DIGEST_LENGTH / 2;
  kj::byte mac[SHA256_DIGEST_LENGTH];
  unsigned int len = SHA256_DIGEST_LENGTH;
  bool ok = HMAC(EVP_sha256(), key, SHA256_DIGEST_LENGTH, base, BASE, mac, &len) == mac &&
      len == SHA256_DIGEST_LENGTH;
  if (ok) {
    memcpy(id, base, BASE);
    memcpy(id + BASE, mac, SHA256_DIGEST_LENGTH - BASE);
  }
  OPENSSL_cleanse(mac, sizeof(mac));
  return ok;
}

long handleActorIdCall(hostcall::Frame& frame);

}  // namespace

long ActorIdFactoryImpl::hostcallHandler(hostcall::Frame& frame) {
  // Nothing may unwind out of a handler into the guest kernel's frames; anything thrown by what
  // the handler calls is reported and becomes a failed call.
  long result = -EIO;
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() { result = handleActorIdCall(frame); })) {
    KJ_LOG(ERROR, "actor-id hostcall handler threw", exception);
    result = -EIO;
  }
  return result;
}

namespace {

long handleActorIdCall(hostcall::Frame& frame) {
  using Op = ActorIdFactoryImpl::Op;
  using Key = ActorIdFactoryImpl::Key;
  constexpr size_t BASE = SHA256_DIGEST_LENGTH / 2;
  auto op = static_cast<Op>(frame.args[0]);
  uint64_t handle = frame.args[1];
  size_t inLen = frame.inLen;
  if (inLen > hostcall::BYTES) return -E2BIG;
  frame.outLen = 0;

  // The key, through the table; a handle naming anything else is refused here.
  auto* key = static_cast<const Key*>(hostMemory::resolveSlot(handle));
  if (key == nullptr || key->tag != ActorIdFactoryImpl::KEY_TAG) return -EACCES;

  auto& state = *reinterpret_cast<NameState*>(frame.state);
  switch (op) {
    case Op::MAC: {
      if (inLen != BASE) return -EINVAL;
      kj::byte base[BASE];
      memcpy(base, frame.buf, BASE);
      if (!deriveId(key->bytes, base, frame.buf)) return -EIO;
      frame.outLen = SHA256_DIGEST_LENGTH;
      return 0;
    }
    case Op::NAME: {
      kj::byte hash[SHA256_DIGEST_LENGTH];
      unsigned int len = SHA256_DIGEST_LENGTH;
      if (HMAC(EVP_sha256(), key->bytes, SHA256_DIGEST_LENGTH, frame.buf, inLen, hash, &len) !=
              hash ||
          len != SHA256_DIGEST_LENGTH) {
        return -EIO;
      }
      bool derived = deriveId(key->bytes, hash, frame.buf);
      OPENSSL_cleanse(hash, sizeof(hash));
      if (!derived) return -EIO;
      frame.outLen = SHA256_DIGEST_LENGTH;
      return 0;
    }
    case Op::NAME_BEGIN: {
      // Whatever the state held before is abandoned; a begun sequence that never ends costs
      // nothing but its slot in this thread's state.
      HMAC_CTX_init(&state.ctx);
      if (!HMAC_Init_ex(&state.ctx, key->bytes, SHA256_DIGEST_LENGTH, EVP_sha256(), nullptr) ||
          !HMAC_Update(&state.ctx, frame.buf, inLen)) {
        HMAC_CTX_cleanup(&state.ctx);
        state.tag = 0;
        return -EIO;
      }
      state.tag = NAME_TAG;
      state.handle = handle;
      return 0;
    }
    case Op::NAME_MORE:
    case Op::NAME_END: {
      if (state.tag != NAME_TAG || state.handle != handle) return -EINVAL;
      if (!HMAC_Update(&state.ctx, frame.buf, inLen)) {
        HMAC_CTX_cleanup(&state.ctx);
        state.tag = 0;
        return -EIO;
      }
      if (op == Op::NAME_MORE) return 0;
      kj::byte hash[SHA256_DIGEST_LENGTH];
      unsigned int len = SHA256_DIGEST_LENGTH;
      int ok = HMAC_Final(&state.ctx, hash, &len);
      HMAC_CTX_cleanup(&state.ctx);
      state.tag = 0;
      state.handle = 0;
      if (!ok || len != SHA256_DIGEST_LENGTH) return -EIO;
      bool derived = deriveId(key->bytes, hash, frame.buf);
      OPENSSL_cleanse(hash, sizeof(hash));
      if (!derived) return -EIO;
      frame.outLen = SHA256_DIGEST_LENGTH;
      return 0;
    }
    case Op::NEW_UNIQUE: {
      if (inLen != 0) return -EINVAL;
      kj::byte base[BASE];
      getHostEntropy(base);
      bool derived = deriveId(key->bytes, base, frame.buf);
      OPENSSL_cleanse(base, sizeof(base));
      if (!derived) return -EIO;
      frame.outLen = SHA256_DIGEST_LENGTH;
      return 0;
    }
  }
  return -EINVAL;
}

}  // namespace

const hostcall::Entry ActorIdFactoryImpl::HOSTCALL = {
  hostcall::ACTOR_ID, &ActorIdFactoryImpl::hostcallHandler};

void ActorIdFactoryImpl::installHostcall() {
  hostcall::install(HOSTCALL);
  // Readies the host entropy buffer: its block is allocated on the first draw, which must not
  // be the one a turn's hostcall makes.
  getHostEntropy(nullptr);
}

}  //namespace workerd::server
