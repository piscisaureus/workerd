#pragma once

#include <workerd/io/actor-id.h>
#include <workerd/util/host-memory.h>
#include <workerd/util/hostcall.h>

#include <openssl/sha.h>

#include <kj/refcount.h>

namespace workerd::server {

using kj::byte;

class ActorIdFactoryImpl final: public ActorIdFactory {
 private:
  class SharedKey;

 public:
  ActorIdFactoryImpl(kj::StringPtr uniqueKey);
  // A factory sharing another's key: what cloneWithJurisdiction() makes.
  explicit ActorIdFactoryImpl(kj::Rc<SharedKey> key);

  class ActorIdImpl final: public ActorId {
   public:
    ActorIdImpl(const kj::byte idParam[SHA256_DIGEST_LENGTH], kj::Maybe<kj::String> name);

    kj::ArrayPtr<const byte> getRaw() const {
      return id;
    }

    kj::String toString() const override;
    kj::Maybe<kj::StringPtr> getName() const override;
    kj::Maybe<kj::StringPtr> getJurisdiction() const override;
    bool equals(const ActorId& other) const override;
    kj::Own<ActorId> clone() const override;

    void clearName() {
      name = kj::none;
    }

   private:
    kj::byte id[SHA256_DIGEST_LENGTH];
    kj::Maybe<kj::String> name;
  };

  kj::Own<ActorId> idFromRaw(kj::ArrayPtr<const byte> bytes, kj::Maybe<kj::String> name);

  kj::Own<ActorId> newUniqueId(kj::Maybe<kj::StringPtr> jurisdiction) override;
  kj::Own<ActorId> idFromName(kj::String name) override;
  kj::Own<ActorId> idFromString(kj::String str) override;

  // Like `idFromString()`, but also attaches `name` (recovered from persistent storage) to the
  // resulting ID so that `DurableObjectId.name` is available. The hex ID is still fully validated
  // (its HMAC MAC must match this namespace's key, exactly as in `idFromString()`); only the
  // relationship between `name` and the ID is trusted rather than re-derived, i.e. we do not
  // recompute HMAC(name) to check it hashes to this ID.
  kj::Own<ActorId> idFromStringNamed(kj::String str, kj::Maybe<kj::String> name);
  kj::Own<ActorIdFactory> cloneWithJurisdiction(
      kj::Maybe<kj::StringPtr> maybeJurisdiction) override;
  bool matchesJurisdiction(const ActorId& id) override;

  // The hostcall that performs every keyed operation on a namespace's key (see `Key` below).
  // workerd's main installs it in the guest kernel (hostcall::install) once gk_init() has run;
  // installHostcall() does that and also readies the host entropy buffer the handler draws
  // unique ids from, so that the handler's first use is not its initialization.
  static const hostcall::Entry HOSTCALL;
  static void installHostcall();

  // The namespace's HMAC key, SHA-256 of its unique key. It lives in host memory
  // (util/host-memory.h): a JS turn at guest ring 3, or an escape from one with a read primitive,
  // cannot read it, and the only thing kept outside host memory is a host slot handle, which the
  // guest kernel validates against its own table on every use, so a rewritten handle resolves to
  // nothing rather than to a key at an address the turn chose. The tag is checked by the
  // hostcall handler as well, so a handle rewritten to that of a live host slot holding some
  // other tenant's object is refused too; a handle rewritten to another namespace's key is the
  // ownership residual the slot table cannot close (see memory-classes.md).
  //
  // Every operation that uses the key -- the HMAC that derives an id from a name, the HMAC of an
  // id's first half that is its second half, and the draw of the random first half of a unique
  // id -- runs in the hostcall handler, host-side, with the key resolved through the handle. The
  // callers, newUniqueId() and idFromName(), are synchronous to JS, and idFromString() is both
  // called in-turn and used by the server to validate ids it receives, so all three go through
  // hostcall::call(), which is one VM exit from inside a turn and a direct call from host code,
  // and the same direct call when the guest kernel is off. The random half of a unique id is
  // drawn by the handler from getHostEntropy() (util/host-entropy.h), whose buffer is in host
  // memory, not from the thread-local buffer behind getEntropy(), which a turn can read or
  // overwrite; in predictable mode the caller supplies a counter instead.
  struct WD_MEMORY_CLASS_HOST Key {
    uint64_t tag;  // KEY_TAG, so the handler can tell this slot's object from another tenant's
    kj::byte bytes[SHA256_DIGEST_LENGTH];
  };
  static constexpr uint64_t KEY_TAG = 0x79656b2d64692d61ull;

  // Operations of the hostcall, in args[0]; args[1] is the key's handle.
  enum class Op : uint64_t {
    MAC = 1,         // in: an id's first BASE_LENGTH bytes; out: the full id, second half derived
    NAME = 2,        // in: a name of at most hostcall::BYTES; out: the id derived from it
    NAME_BEGIN = 3,  // in: the first chunk of a longer name; out: nothing (state begun)
    NAME_MORE = 4,   // in: a further chunk; out: nothing
    NAME_END = 5,    // in: the last chunk; out: the id derived from the whole name
    NEW_UNIQUE = 6,  // in: nothing; out: an id with a random first half from host entropy
  };

 private:
  // The slot's owner. A jurisdiction clone of a factory shares the key rather than copying it:
  // copying would read the key, which a turn cannot do, and the clone is made in-turn.
  class SharedKey final: public kj::Refcounted {
   public:
    explicit SharedKey(HostSlot<Key> slot): slot(kj::mv(slot)) {}
    HostSlot<Key> slot;
  };
  kj::Rc<SharedKey> key;

  uint64_t counter = 0;  // only used in predictable mode

  static constexpr size_t BASE_LENGTH = SHA256_DIGEST_LENGTH / 2;

  static long hostcallHandler(hostcall::Frame& frame);

  // Runs one operation through the hostcall with `in` as the bytes in and fills `id` with the
  // SHA256_DIGEST_LENGTH bytes out. `frame` carries the per-sequence state for the NAME_* ops.
  void keyedOp(
      hostcall::Frame& frame, Op op, kj::ArrayPtr<const byte> in, kj::Maybe<kj::ArrayPtr<byte>> id);
};

}  // namespace workerd::server
