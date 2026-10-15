// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative fixtures for workerd-memory-class: none of these may produce a
// diagnostic.

#define WD_MEMORY_CLASS_ISOLATE __attribute__((annotate("workerd_memory_class_isolate")))
#define WD_MEMORY_CLASS_HOST __attribute__((annotate("workerd_memory_class_host")))
#define WD_MEMORY_CLASS_BOUNDARY __attribute__((annotate("workerd_memory_class_boundary")))

namespace kj {
template <typename T>
class Own {};
template <typename T>
class Arc {};
template <typename T>
class Maybe {};
template <typename T>
class Array {};
template <typename T>
class Promise {};
class TaskSet {};
class Canceler {};
template <typename T>
class MutexGuarded {};
class String {
 public:
  ~String();
};
template <typename T>
class ArrayPtr {
 public:
  T* ptr;
  unsigned long size;
};
}  // namespace kj

namespace v8 {
class Value {};
template <typename T>
class Global {};
}  // namespace v8

namespace workerd::jsg {
template <typename T>
class Ref {};
template <typename T>
class V8Ref {};
using Value = V8Ref<v8::Value>;
template <typename T>
class Promise {};
// A name that merely ends in a listed name.
class RefCount {};
}  // namespace workerd::jsg

// Same-named templates in an unrelated namespace are not the listed types.
namespace other {
template <typename T>
class Promise {};
namespace jsg {
template <typename T>
class Ref {};
}  // namespace jsg
}  // namespace other

namespace workerd {

class WD_MEMORY_CLASS_HOST HostThing {
 public:
  int x;
};

class WD_MEMORY_CLASS_ISOLATE IsolateThing {
 public:
  int y;
};

class Widget {};
class Unclassified {};

// Case N1: an unannotated class is never checked, whatever it holds.
class Unannotated {
  jsg::Ref<Widget> ref;
  kj::Promise<void> promise;
  IsolateThing* isolate;
  kj::Own<HostThing> host;
  char* data;
};

// Case N2: a host class holding host-side state, other host objects by any
// means, unclassified objects, and plain values.
class WD_MEMORY_CLASS_HOST HostClean {
  HostClean(HostThing& other): other(other) {}
  kj::Promise<void> promise;
  kj::TaskSet tasks;
  kj::Canceler canceler;
  kj::Own<HostThing> owned;
  kj::Maybe<HostThing&> maybeOther;
  HostThing& other;
  HostThing* pointer;
  kj::Own<Unclassified> unclassified;
  kj::String name;
  unsigned long handle;
  jsg::RefCount refCount;
};

// Case N3: an isolate class holding V8 handles, other isolate objects by any
// means, unclassified objects, and plain values.
class WD_MEMORY_CLASS_ISOLATE IsolateClean {
  IsolateClean(IsolateThing& other): other(other) {}
  jsg::Ref<Widget> ref;
  jsg::Value value;
  v8::Global<v8::Value> global;
  kj::Maybe<jsg::Ref<Widget>> maybeRef;
  kj::Array<jsg::Ref<Widget>> refs;
  kj::Own<IsolateThing> owned;
  IsolateThing& other;
  IsolateThing* pointer;
  kj::Own<Unclassified> unclassified;
  kj::String name;
  other::Promise<int> notKjPromise;
  other::jsg::Ref<Widget> notJsgRef;
  unsigned long handle;
};

// Case N4: a boundary record of plain values, arrays of them, and other
// boundary records.
enum class Kind : unsigned char { A, B };
enum LegacyKind { LEGACY_A, LEGACY_B };

struct WD_MEMORY_CLASS_BOUNDARY Descriptor {
  unsigned long handle;
  unsigned int offset;
  unsigned int length;
};

struct WD_MEMORY_CLASS_BOUNDARY Message {
  unsigned int status;
  Kind kind;
  LegacyKind legacyKind;
  bool flag;
  double ratio;
  char description[64];
  Descriptor body;
  Descriptor chunks[4];
  union {
    unsigned long handle;
    unsigned int index;
  };
};

// Case N5: a boundary record deriving from a boundary record.
struct WD_MEMORY_CLASS_BOUNDARY ExtendedMessage: public Message {
  unsigned int extra;
};

// Case N6: a host class deriving from a host class and from an unclassified
// class; an isolate class likewise.
class WD_MEMORY_CLASS_HOST HostDerived: public HostThing, public Unclassified {};
class WD_MEMORY_CLASS_ISOLATE IsolateDerived: public IsolateThing, public Unclassified {};

// Case N7: a class template whose member type is a parameter, instantiated
// only with types that belong on its side.
template <typename T>
class WD_MEMORY_CLASS_ISOLATE IsolateHolder {
  T value;
};
IsolateHolder<int> holderOfInt;
IsolateHolder<jsg::Ref<Widget>> holderOfRef;
IsolateHolder<kj::Own<IsolateThing>> holderOfOwn;

template <typename T>
class WD_MEMORY_CLASS_HOST HostHolder {
  T value;
};
HostHolder<kj::Promise<void>> holderOfPromise;

// Case N8: a deliberate crossing is suppressed explicitly, with a reason.
class WD_MEMORY_CLASS_HOST HostSuppressed {
  // The split for this class keeps the ref on the host side until the handle
  // table lands.
  jsg::Ref<Widget> ref;  // NOLINT(workerd-memory-class)
};

// NOLINTBEGIN(workerd-memory-class)
class WD_MEMORY_CLASS_ISOLATE IsolateSuppressed {
  kj::Promise<void> promise;
};
// NOLINTEND(workerd-memory-class)

}  // namespace workerd
