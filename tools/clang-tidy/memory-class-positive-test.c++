// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive fixtures for workerd-memory-class: each case must produce exactly
// one diagnostic; memory-class-test.sh asserts the exact total.

#define WD_MEMORY_CLASS_ISOLATE __attribute__((annotate("workerd_memory_class_isolate")))
#define WD_MEMORY_CLASS_HOST __attribute__((annotate("workerd_memory_class_host")))
#define WD_MEMORY_CLASS_BOUNDARY __attribute__((annotate("workerd_memory_class_boundary")))

namespace kj {
template <typename T>
class Own {};
template <typename T>
class Arc {};
template <typename T>
class Rc {};
template <typename T>
class Maybe {};
template <typename T>
class Array {};
template <typename T>
class Promise {};
template <typename T>
class ForkedPromise {};
class TaskSet {};
template <typename T>
class PromiseFulfiller {};
class Canceler {};
template <typename T>
class MutexGuarded {};
class Executor {};
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
template <typename T>
class TracedReference {};
}  // namespace v8

namespace workerd::jsg {
template <typename T>
class Ref {};
template <typename T>
class V8Ref {};
using Value = V8Ref<v8::Value>;
template <typename T>
class JsRef {};
template <typename T>
class Promise {};
template <typename T>
class Function {};
class Data {};
class JsContext {};
}  // namespace workerd::jsg

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

// Case P1: a host class holding a V8 handle through jsg::Ref.
class WD_MEMORY_CLASS_HOST HostWithRef {
  jsg::Ref<Widget> ref;
};

// Case P2: a host class embedding an isolate class by value.
class WD_MEMORY_CLASS_HOST HostWithIsolateValue {
  IsolateThing thing;
};

// Case P3: a host class holding an isolate-side type inside a container.
class WD_MEMORY_CLASS_HOST HostWithMaybeRef {
  kj::Maybe<jsg::Ref<Widget>> maybeRef;
};

// Case P4: a host class holding a raw pointer to an isolate class.
class WD_MEMORY_CLASS_HOST HostWithIsolatePointer {
  IsolateThing* ptr;
};

// Case P5: a host class reaching a V8 handle through a type alias.
class WD_MEMORY_CLASS_HOST HostWithValue {
  jsg::Value value;
};

// Case P6: an isolate class holding a KJ promise.
class WD_MEMORY_CLASS_ISOLATE IsolateWithPromise {
  kj::Promise<void> promise;
};

// Case P7: an isolate class owning a host class; the disposer would live in
// the arena.
class WD_MEMORY_CLASS_ISOLATE IsolateWithOwn {
  kj::Own<HostThing> own;
};

// Case P8: the same through two layers of wrapper.
class WD_MEMORY_CLASS_ISOLATE IsolateWithMaybeArc {
  kj::Maybe<kj::Arc<HostThing>> maybeArc;
};

// Case P9: an isolate class holding a reference to a host class.
class WD_MEMORY_CLASS_ISOLATE IsolateWithHostReference {
  IsolateWithHostReference(HostThing& host): host(host) {}
  HostThing& host;
};

// Case P10: a host-side type hidden behind a typedef.
using BackgroundTask = kj::Promise<int>;
class WD_MEMORY_CLASS_ISOLATE IsolateWithTypedef {
  BackgroundTask task;
};

// Case P11: a host-side type in a class template, decided on the template
// and therefore reported once however many times it is instantiated.
template <typename T>
class WD_MEMORY_CLASS_ISOLATE IsolateTemplateWithPromise {
  kj::Promise<T> promise;
};
IsolateTemplateWithPromise<int> instantiatedWithInt;
IsolateTemplateWithPromise<void> instantiatedWithVoid;

// Case P12: a member whose type is a template parameter, decided on the
// instantiation; only the violating instantiation is reported.
template <typename T>
class WD_MEMORY_CLASS_ISOLATE IsolateHolder {
  T value;
};
IsolateHolder<int> holderOfInt;
IsolateHolder<kj::TaskSet> holderOfTaskSet;

// Case P13: an isolate class holding a host mutex.
class WD_MEMORY_CLASS_ISOLATE IsolateWithMutex {
  kj::MutexGuarded<int> guarded;
};

// Case P14: a host class deriving from an isolate class.
class WD_MEMORY_CLASS_HOST HostDerivedFromIsolate: public IsolateThing {};

// Case P15: a boundary record holding a pointer.
struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithPointer {
  unsigned int length;
  char* data;
};

// Case P16: a boundary record holding a reference.
struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithReference {
  BoundaryWithReference(int& target): target(target) {}
  int& target;
};

// Case P17: a boundary record holding something with a destructor.
struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithString {
  kj::String description;
};

// Case P18: a boundary record holding a trivially destructible class that is
// not itself a boundary record (and which hides a pointer).
struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithArrayPtr {
  kj::ArrayPtr<unsigned char> bytes;
};

// Case P19: a boundary record holding an array of pointers.
struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithPointerArray {
  const char* names[4];
};

// Case P20: a boundary record with a vtable of its own.
struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithVirtual {
  unsigned int status;
  virtual void finish();
};

// Case P21: a boundary record deriving from a non-boundary class.
struct PlainBase {
  int a;
};
struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithPlainBase: public PlainBase {
  int b;
};

// Case P22: a boundary record whose unnamed union hides a pointer.
struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithUnion {
  unsigned int kind;
  union {
    unsigned long handle;
    void* object;
  };
};

// Case P23: a host class holding an array of isolate-side handles.
class WD_MEMORY_CLASS_HOST HostWithGlobalArray {
  v8::Global<v8::Value> slots[2];
};

}  // namespace workerd
