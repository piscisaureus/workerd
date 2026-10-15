#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/memory-class-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/memory-class-negative-test.c++"
readonly CHECK="workerd-memory-class"
readonly CHECKS="-*,${CHECK}"

set +e
positive_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${POSITIVE}" -- -std=c++23 2>&1)
positive_status=$?
set -e

if [[ ${positive_status} -eq 0 ]]; then
  printf '%s\n' "Expected ${CHECK} to reject the positive fixtures." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

# Asserts that a diagnostic with the given message is reported on the given
# fixture line, identifying the case that produced it.
expect_diag_at() {
  local line="$1"
  local message="$2"
  local description="$3"
  if ! grep -qF "memory-class-positive-test.c++:${line}:" <<<"${positive_output}"; then
    printf 'Missing expected diagnostic (%s) on line %s\n' "${description}" "${line}" >&2
    printf '%s\n' "${positive_output}" >&2
    exit 1
  fi
  if ! grep -F "memory-class-positive-test.c++:${line}:" <<<"${positive_output}" |
    grep -qF "${message}"; then
    printf 'Diagnostic on line %s (%s) has the wrong message; expected: %s\n' \
      "${line}" "${description}" "${message}" >&2
    printf '%s\n' "${positive_output}" >&2
    exit 1
  fi
}

# Resolves the line of a fixture case from a unique snippet of its source, so
# the assertions below do not depend on hard-coded line numbers.
line_of() {
  local needle="$1"
  local line
  line=$(grep -nF -- "${needle}" "${POSITIVE}" | head -n1 | cut -d: -f1)
  if [[ -z "${line}" ]]; then
    printf 'Fixture snippet not found: %s\n' "${needle}" >&2
    exit 1
  fi
  printf '%s' "${line}"
}

readonly HOST_WHY="a host object names isolate state only by handle"
readonly ISOLATE_WHY="an isolate object reaches host state only through a handle or a message"
readonly BOUNDARY_WHY="a boundary record carries only integers, enums, bools, floats, arrays of those and other boundary records"

expect_diag_at "$(line_of '  jsg::Ref<Widget> ref;')" \
  "field 'ref' of host class 'workerd::HostWithRef' has isolate-side type 'jsg::Ref<Widget>'; ${HOST_WHY}" \
  "P1: jsg::Ref in a host class"
expect_diag_at "$(line_of '  IsolateThing thing;')" \
  "field 'thing' of host class 'workerd::HostWithIsolateValue' embeds isolate class 'workerd::IsolateThing'; ${HOST_WHY}" \
  "P2: isolate class by value in a host class"
expect_diag_at "$(line_of '  kj::Maybe<jsg::Ref<Widget>> maybeRef;')" \
  "field 'maybeRef' of host class 'workerd::HostWithMaybeRef' has type 'kj::Maybe<jsg::Ref<Widget>>', which holds isolate-side type 'jsg::Ref<Widget>'; ${HOST_WHY}" \
  "P3: jsg::Ref inside kj::Maybe in a host class"
expect_diag_at "$(line_of '  IsolateThing* ptr;')" \
  "field 'ptr' of host class 'workerd::HostWithIsolatePointer' is a pointer to isolate class 'workerd::IsolateThing'; ${HOST_WHY}" \
  "P4: pointer to an isolate class in a host class"
expect_diag_at "$(line_of '  jsg::Value value;')" \
  "field 'value' of host class 'workerd::HostWithValue' has isolate-side type 'jsg::Value'; ${HOST_WHY}" \
  "P5: V8 handle through a type alias in a host class"
expect_diag_at "$(line_of '  kj::Promise<void> promise;')" \
  "field 'promise' of isolate class 'workerd::IsolateWithPromise' has host-side type 'kj::Promise<void>'; ${ISOLATE_WHY}" \
  "P6: kj::Promise in an isolate class"
expect_diag_at "$(line_of '  kj::Own<HostThing> own;')" \
  "field 'own' of isolate class 'workerd::IsolateWithOwn' has type 'kj::Own<HostThing>', which owns host class 'workerd::HostThing'; ${ISOLATE_WHY}" \
  "P7: kj::Own of a host class in an isolate class"
expect_diag_at "$(line_of '  kj::Maybe<kj::Arc<HostThing>> maybeArc;')" \
  "field 'maybeArc' of isolate class 'workerd::IsolateWithMaybeArc' has type 'kj::Maybe<kj::Arc<HostThing>>', which owns host class 'workerd::HostThing'; ${ISOLATE_WHY}" \
  "P8: kj::Arc of a host class inside kj::Maybe in an isolate class"
expect_diag_at "$(line_of '  HostThing& host;')" \
  "field 'host' of isolate class 'workerd::IsolateWithHostReference' is a reference to host class 'workerd::HostThing'; ${ISOLATE_WHY}" \
  "P9: reference to a host class in an isolate class"
expect_diag_at "$(line_of '  BackgroundTask task;')" \
  "field 'task' of isolate class 'workerd::IsolateWithTypedef' has host-side type 'BackgroundTask'; ${ISOLATE_WHY}" \
  "P10: kj::Promise behind a typedef in an isolate class"
expect_diag_at "$(line_of '  kj::Promise<T> promise;')" \
  "field 'promise' of isolate class 'workerd::IsolateTemplateWithPromise' has host-side type 'kj::Promise<T>'; ${ISOLATE_WHY}" \
  "P11: kj::Promise<T> in an isolate class template, reported on the template"
expect_diag_at "$(line_of '  T value;')" \
  "field 'value' of isolate class 'workerd::IsolateHolder<kj::TaskSet>' has host-side type 'kj::TaskSet'; ${ISOLATE_WHY}" \
  "P12: template parameter member, reported on the violating instantiation"
expect_diag_at "$(line_of '  kj::MutexGuarded<int> guarded;')" \
  "field 'guarded' of isolate class 'workerd::IsolateWithMutex' has host-side type 'kj::MutexGuarded<int>'; ${ISOLATE_WHY}" \
  "P13: kj::MutexGuarded in an isolate class"
expect_diag_at "$(line_of 'class WD_MEMORY_CLASS_HOST HostDerivedFromIsolate: public IsolateThing {};')" \
  "host class 'workerd::HostDerivedFromIsolate' derives from isolate class 'workerd::IsolateThing'; ${HOST_WHY}" \
  "P14: host class deriving from an isolate class"
expect_diag_at "$(line_of '  char* data;')" \
  "field 'data' of boundary class 'workerd::BoundaryWithPointer' is a pointer ('char *'); ${BOUNDARY_WHY}" \
  "P15: pointer in a boundary record"
expect_diag_at "$(line_of '  int& target;')" \
  "field 'target' of boundary class 'workerd::BoundaryWithReference' is a reference ('int &'); ${BOUNDARY_WHY}" \
  "P16: reference in a boundary record"
expect_diag_at "$(line_of '  kj::String description;')" \
  "field 'description' of boundary class 'workerd::BoundaryWithString' has type 'kj::String', which has a non-trivial destructor; ${BOUNDARY_WHY}" \
  "P17: kj::String in a boundary record"
expect_diag_at "$(line_of '  kj::ArrayPtr<unsigned char> bytes;')" \
  "field 'bytes' of boundary class 'workerd::BoundaryWithArrayPtr' has type 'kj::ArrayPtr<unsigned char>', which is not a boundary class; ${BOUNDARY_WHY}" \
  "P18: trivially destructible non-boundary class in a boundary record"
expect_diag_at "$(line_of '  const char* names[4];')" \
  "field 'names' of boundary class 'workerd::BoundaryWithPointerArray' is an array of 'const char *', which is a pointer; ${BOUNDARY_WHY}" \
  "P19: array of pointers in a boundary record"
expect_diag_at "$(line_of 'struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithVirtual {')" \
  "boundary class 'workerd::BoundaryWithVirtual' has a vtable; ${BOUNDARY_WHY}" \
  "P20: boundary record with a virtual function"
expect_diag_at "$(line_of 'struct WD_MEMORY_CLASS_BOUNDARY BoundaryWithPlainBase: public PlainBase {')" \
  "boundary class 'workerd::BoundaryWithPlainBase' derives from 'workerd::PlainBase', which is not a boundary class; ${BOUNDARY_WHY}" \
  "P21: boundary record deriving from a non-boundary class"
expect_diag_at "$(line_of '    void* object;')" \
  "field 'object' of boundary class 'workerd::BoundaryWithUnion' is a pointer ('void *'); ${BOUNDARY_WHY}" \
  "P22: pointer inside an unnamed union in a boundary record"
expect_diag_at "$(line_of '  v8::Global<v8::Value> slots[2];')" \
  "field 'slots' of host class 'workerd::HostWithGlobalArray' has type 'v8::Global<v8::Value>[2]', which holds isolate-side type 'v8::Global<v8::Value>'; ${HOST_WHY}" \
  "P23: array of V8 handles in a host class"

# Exact-count lock: one diagnostic per positive case, no more, no fewer.
expected_count=23
actual_count=$(grep -c "\[${CHECK}" <<<"${positive_output}" || true)
if [[ "${actual_count}" -ne "${expected_count}" ]]; then
  printf 'Expected exactly %s %s diagnostics, got %s\n' \
    "${expected_count}" "${CHECK}" "${actual_count}" >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

negative_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${NEGATIVE}" -- -std=c++23 2>&1)

if [[ "${negative_output}" == *"${CHECK}"* ]]; then
  printf '%s\n' "Expected the negative fixtures to be accepted." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi
