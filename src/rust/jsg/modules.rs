// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "registers module callbacks with the C++ registry"
)]

use std::pin::Pin;

use crate::Lock;
use crate::v8;
use crate::v8::ffi;
pub use crate::v8::ffi::ModuleType;

/// Callback that produces a builtin module's exports object.
///
/// The callback runs in the module registry's current scope, so it receives a
/// [`Lock`] for that scope and returns a handle branded with the same scope.
pub type BuiltinModuleCallback = for<'s> fn(&mut Lock<'s>) -> v8::Local<'s, v8::Value>;

/// Registers a builtin module with the given specifier and module type.
///
/// The callback is invoked when JavaScript imports the module and returns the
/// module's exports.
pub fn add_builtin(
    registry: Pin<&mut ffi::ModuleRegistry>,
    specifier: &str,
    callback: BuiltinModuleCallback,
    module_type: ModuleType,
) {
    // SAFETY: `trampoline` is called back by C++ with the isolate that evaluates the
    // module and with `data` carrying `callback`, which is a plain fn pointer and
    // therefore valid for the life of the program.
    unsafe {
        ffi::register_add_builtin_module(
            registry,
            specifier,
            trampoline,
            callback as usize,
            module_type,
        );
    }
}

/// Recovers the [`BuiltinModuleCallback`] from `data` and runs it in the current scope.
///
/// Called only by the C++ module registry, with the `data` that [`add_builtin`]
/// registered, while the isolate is locked, a context is entered and a `HandleScope`
/// is open. It is not an `unsafe fn` because cxx function pointers cannot be.
fn trampoline(isolate: *mut ffi::Isolate, data: usize) -> ffi::Local {
    // SAFETY: `add_builtin` stored a `BuiltinModuleCallback` in `data`; fn pointers
    // and `usize` have the same size on all supported targets.
    let callback = unsafe { std::mem::transmute::<usize, BuiltinModuleCallback>(data) };
    // SAFETY: the C++ registry calls this with the isolate locked, in a context and inside
    // a HandleScope that outlives the call.
    crate::callback_scope!(unsafe let lock, isolate);
    // SAFETY: the handle lives in the registry's HandleScope, which outlives this call.
    unsafe { callback(lock).into_ffi() }
}
