// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Handle scopes and the [`Lock`]s they hand out.
//!
//! Every V8 handle lives in a `v8::HandleScope` and dies when that scope closes. A
//! [`Lock<'s>`](Lock) stands for the innermost open scope. It is only ever handed out as
//! `&'s mut Lock<'s>`, borrowed from the pinned storage of its scope, and every API that
//! creates a handle returns a [`v8::Local<'s, _>`](crate::v8::Local), so the borrow checker
//! rejects any use of a handle after its scope closed.
//!
//! Scopes are opened by macros that pin their storage on the stack and shadow it with the
//! lock, as in `rusty_v8`:
//!
//! ```ignore
//! fn make<'s>(lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Object> {
//!     let outer = lock.new_object();
//!     {
//!         jsg::scope!(let lock, lock);
//!         // Handles created here die at the end of the block; `outer` is still usable.
//!         let tmp = lock.new_object();
//!         outer.set(lock, "tmp", tmp.into());
//!     }
//!     jsg::escapable_handle_scope!(let lock, lock);
//!     let obj = lock.new_object();
//!     lock.escape(obj)
//! }
//! ```
//!
//! A nested scope mutably borrows the lock it was opened from until the scope closes, so
//! the outer lock cannot create handles (which would land in the nested scope) in the
//! meantime. Handles of the outer scope can be used with the nested lock.
//!
//! C++ and V8 enter Rust through [`callback_scope!`], which is `unsafe`: it hands out a
//! lock for a `HandleScope` the caller already has open.
//!
//! ### Isolates
//!
//! Lifetimes tie a handle to its scope, not to that scope's isolate. To keep handles of one
//! isolate out of another, all scopes open on a thread at the same time must belong to one
//! isolate: opening a scope of another isolate while one is open panics. A handle cannot
//! outlive its scope, so every live handle belongs to the isolate of any lock it can be
//! used with. Code that needs scopes of two isolates open at once can open the second
//! isolate's scopes inside the `unsafe` [`allow_scopes_of_another_isolate`].
//!
//! Handles that outlive scopes remember their isolate and panic when used in another:
//! [`v8::Global`](crate::v8::Global) and the JavaScript wrapper of an [`Rc`](crate::Rc).
//!
//! A scope whose storage is leaked keeps counting as open, so its isolate keeps owning the
//! thread.

use std::marker::PhantomData;
use std::marker::PhantomPinned;
use std::mem::MaybeUninit;
use std::num::NonZeroU64;
use std::ops::Deref;
use std::ops::DerefMut;
use std::pin::Pin;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;

use crate::v8;

/// Proof that the V8 isolate is locked by the current thread, that a context is entered,
/// and that the `HandleScope` identified by `'s` is the innermost open one.
///
/// Mirrors `jsg::Lock&` in C++ JSG. User code never constructs a `Lock`: [`scope!`],
/// [`escapable_handle_scope!`] and [`callback_scope!`] hand out `&'s mut Lock<'s>`, where
/// `'s` is the borrow of the scope's pinned storage. Every handle created through a lock is
/// a [`v8::Local<'s, _>`](v8::Local) and cannot outlive that borrow.
///
/// `'s` is invariant, and `Lock` is neither `Send` nor `Sync`.
pub struct Lock<'s> {
    isolate: v8::IsolatePtr,
    _brand: PhantomData<*mut &'s ()>,
}

impl Lock<'_> {
    fn new(isolate: v8::IsolatePtr) -> Self {
        Self {
            isolate,
            _brand: PhantomData,
        }
    }

    /// Returns the isolate associated with this lock.
    pub fn isolate(&self) -> v8::IsolatePtr {
        self.isolate
    }

    /// Returns the identity of this lock's isolate.
    pub(crate) fn isolate_id(&mut self) -> IsolateId {
        self.realm().id()
    }
}

/// Identifies an isolate. Unlike its address, an `IsolateId` is never reused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IsolateId(NonZeroU64);

impl IsolateId {
    pub fn next() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        Self(NonZeroU64::MIN.saturating_add(NEXT.fetch_add(1, Ordering::Relaxed)))
    }
}

/// Tracks which isolate owns the scopes open on this thread.
pub mod scope_owner {
    use std::cell::Cell;
    use std::ptr;

    use crate::v8;

    #[derive(Clone, Copy)]
    struct State {
        isolate: *mut v8::ffi::Isolate,
        depth: usize,
    }

    const NONE: State = State {
        isolate: ptr::null_mut(),
        depth: 0,
    };

    thread_local! {
        static STATE: Cell<State> = const { Cell::new(NONE) };
    }

    /// Records that a scope of `isolate` was opened.
    ///
    /// # Panics
    /// Panics if a scope of another isolate is open on this thread.
    pub fn enter(isolate: v8::IsolatePtr) {
        STATE.with(|state| {
            let mut s = state.get();
            if s.depth == 0 {
                s = State {
                    isolate: isolate.as_ffi(),
                    ..NONE
                };
            } else {
                assert!(
                    s.isolate == isolate.as_ffi(),
                    "attempt to open a scope of an isolate while a scope of another isolate is \
                     open on this thread"
                );
            }
            s.depth += 1;
            state.set(s);
        });
    }

    /// Records that a scope opened with [`enter`] was closed.
    pub fn exit() {
        STATE.with(|state| {
            let mut s = state.get();
            debug_assert!(s.depth > 0, "scope_owner::exit() without enter()");
            s.depth -= 1;
            state.set(s);
        });
    }

    /// Runs `f` as if no scope were open on this thread, and restores the open scopes after.
    pub fn suspend<R>(f: impl FnOnce() -> R) -> R {
        struct Restore(State);
        impl Drop for Restore {
            fn drop(&mut self) {
                STATE.with(|state| {
                    debug_assert_eq!(
                        state.get().depth,
                        0,
                        "a scope opened inside allow_scopes_of_another_isolate() is still open"
                    );
                    state.set(self.0);
                });
            }
        }
        let _restore = Restore(STATE.with(|state| state.replace(NONE)));
        f()
    }
}

/// Runs `f` with the scopes open on this thread set aside, so `f` can open scopes of
/// another isolate. They count as open again once `f` returns.
///
/// # Safety
///
/// While `f` runs, handles of the isolate whose scopes were set aside must not be used with
/// any other isolate, and handles that `f` creates must not be used once it returns. `f`
/// must close every scope it opens, and must not close any scope that was open when it was
/// called.
#[expect(
    unsafe_code,
    reason = "lifts the one-isolate-per-thread rule that keeps handles in their isolate"
)]
pub unsafe fn allow_scopes_of_another_isolate<R>(f: impl FnOnce() -> R) -> R {
    scope_owner::suspend(f)
}

/// Words reserved for a `v8::HandleScope`; `ffi.c++` checks that it fits.
const HANDLE_SCOPE_WORDS: usize = 4;
/// Words reserved for a `v8::EscapableHandleScope`; `ffi.c++` checks that it fits.
const ESCAPABLE_HANDLE_SCOPE_WORDS: usize = 5;

/// Storage of a `v8::HandleScope` nested in another scope. Use [`scope!`] to open one.
pub struct HandleScope<'p> {
    lock: Lock<'static>,
    raw: [MaybeUninit<usize>; HANDLE_SCOPE_WORDS],
    open: bool,
    _parent: PhantomData<&'p mut ()>,
    _pinned: PhantomPinned,
}

#[expect(unsafe_code, reason = "constructs a C++ HandleScope in pinned storage")]
impl<'p> HandleScope<'p> {
    /// Creates the storage of a scope nested in the scope of `parent`, which stays borrowed
    /// until the storage is dropped.
    pub fn new(parent: &'p mut Lock<'_>) -> Self {
        Self {
            lock: Lock::new(parent.isolate),
            raw: [MaybeUninit::uninit(); HANDLE_SCOPE_WORDS],
            open: false,
            _parent: PhantomData,
            _pinned: PhantomPinned,
        }
    }

    /// Opens the scope and returns its lock. Opening it again closes it first.
    ///
    /// # Panics
    /// Panics if a scope of another isolate is open on this thread.
    pub fn open<'s>(self: Pin<&'s mut Self>) -> &'s mut Lock<'s> {
        // SAFETY: nothing is moved out of the storage.
        let this = unsafe { self.get_unchecked_mut() };
        this.close();
        scope_owner::enter(this.lock.isolate);
        // SAFETY: the isolate is locked (the parent lock proves it) and `raw` is pinned and
        // large enough; `close` destroys the scope before the storage goes away.
        unsafe {
            v8::ffi::handle_scope_open(this.lock.isolate.as_ffi(), this.raw.as_mut_ptr() as usize);
        }
        this.open = true;
        // SAFETY: `Lock<'s>` differs from `Lock<'static>` only in its brand. The parent stays
        // borrowed until the storage is dropped, so this scope stays innermost while 's lasts.
        unsafe { &mut *(&raw mut this.lock).cast::<Lock<'s>>() }
    }

    fn close(&mut self) {
        if self.open {
            self.open = false;
            // SAFETY: `open` constructed the scope in `raw`, and the scopes nested in it, which
            // borrowed its lock, are closed.
            unsafe { v8::ffi::handle_scope_close(self.raw.as_mut_ptr() as usize) };
            scope_owner::exit();
        }
    }
}

impl Drop for HandleScope<'_> {
    fn drop(&mut self) {
        self.close();
    }
}

/// Storage of a `v8::EscapableHandleScope` nested in another scope. Use
/// [`escapable_handle_scope!`] to open one.
pub struct EscapableHandleScope<'p, 'e> {
    lock: EscapableLock<'static, 'static>,
    raw: [MaybeUninit<usize>; ESCAPABLE_HANDLE_SCOPE_WORDS],
    open: bool,
    _parent: PhantomData<&'p mut Lock<'e>>,
    _pinned: PhantomPinned,
}

/// The lock of an [`EscapableHandleScope`]: a [`Lock<'s>`](Lock) for the scope itself that
/// can escape one handle into the enclosing scope `'e`.
pub struct EscapableLock<'s, 'e> {
    lock: Lock<'s>,
    raw: usize,
    escaped: bool,
    _escape: PhantomData<*mut &'e ()>,
}

#[expect(
    unsafe_code,
    reason = "constructs a C++ EscapableHandleScope in pinned storage"
)]
impl<'p, 'e> EscapableHandleScope<'p, 'e> {
    /// Creates the storage of a scope nested in the scope of `parent`, which stays borrowed
    /// until the storage is dropped.
    pub fn new(parent: &'p mut Lock<'e>) -> Self {
        Self {
            lock: EscapableLock {
                lock: Lock::new(parent.isolate),
                raw: 0,
                escaped: false,
                _escape: PhantomData,
            },
            raw: [MaybeUninit::uninit(); ESCAPABLE_HANDLE_SCOPE_WORDS],
            open: false,
            _parent: PhantomData,
            _pinned: PhantomPinned,
        }
    }

    /// Opens the scope and returns its lock. Opening it again closes it first.
    ///
    /// # Panics
    /// Panics if a scope of another isolate is open on this thread.
    pub fn open<'s>(self: Pin<&'s mut Self>) -> &'s mut EscapableLock<'s, 'e> {
        // SAFETY: nothing is moved out of the storage.
        let this = unsafe { self.get_unchecked_mut() };
        this.close();
        scope_owner::enter(this.lock.lock.isolate);
        // SAFETY: as in `HandleScope::open`. V8 allocates the escape slot in the parent scope.
        unsafe {
            v8::ffi::escapable_handle_scope_open(
                this.lock.lock.isolate.as_ffi(),
                this.raw.as_mut_ptr() as usize,
            );
        }
        this.open = true;
        this.lock.raw = this.raw.as_mut_ptr() as usize;
        this.lock.escaped = false;
        // SAFETY: as in `HandleScope::open`; `'e` is the parent's brand, which `new` recorded.
        unsafe { &mut *(&raw mut this.lock).cast::<EscapableLock<'s, 'e>>() }
    }

    fn close(&mut self) {
        if self.open {
            self.open = false;
            // SAFETY: as in `HandleScope::close`.
            unsafe { v8::ffi::escapable_handle_scope_close(self.raw.as_mut_ptr() as usize) };
            scope_owner::exit();
        }
    }
}

impl Drop for EscapableHandleScope<'_, '_> {
    fn drop(&mut self) {
        self.close();
    }
}

#[expect(
    unsafe_code,
    reason = "escapes a handle through the C++ EscapableHandleScope"
)]
impl<'s, 'e> EscapableLock<'s, 'e> {
    /// Copies `value` into the enclosing scope and returns the copy.
    ///
    /// # Panics
    /// Panics if called twice on the same scope.
    pub fn escape<T>(&mut self, value: v8::Local<'s, T>) -> v8::Local<'e, T> {
        assert!(!self.escaped, "EscapableLock::escape() called twice");
        self.escaped = true;
        // SAFETY: `raw` holds the open scope (the lock is only reachable while it is open),
        // and the escaped handle lives in the slot V8 allocated in the enclosing scope `'e`.
        unsafe {
            let escaped = v8::ffi::escapable_handle_scope_escape(self.raw, value.into_ffi());
            v8::Local::from_ffi(self.lock.isolate, escaped)
        }
    }
}

impl<'s> Deref for EscapableLock<'s, '_> {
    type Target = Lock<'s>;

    fn deref(&self) -> &Lock<'s> {
        &self.lock
    }
}

impl<'s> DerefMut for EscapableLock<'s, '_> {
    fn deref_mut(&mut self) -> &mut Lock<'s> {
        &mut self.lock
    }
}

/// What a [`CallbackScope`] can be created from.
pub trait CallbackScopeParam: callback_scope_param::Sealed {
    /// # Safety
    /// See [`CallbackScope::new`].
    #[doc(hidden)]
    #[expect(unsafe_code, reason = "reads the isolate from a raw pointer")]
    unsafe fn into_isolate(self) -> v8::IsolatePtr;
}

mod callback_scope_param {
    pub trait Sealed {}
    impl Sealed for *mut crate::v8::ffi::Isolate {}
    impl Sealed for *mut crate::v8::ffi::FunctionCallbackInfo {}
}

#[expect(unsafe_code, reason = "reads the isolate from a raw pointer")]
impl CallbackScopeParam for *mut v8::ffi::Isolate {
    /// # Panics
    /// Panics if the isolate is not locked by this thread or no context is entered: a C++
    /// caller bug that must not reach V8.
    unsafe fn into_isolate(self) -> v8::IsolatePtr {
        // SAFETY: the isolate is valid (guaranteed by caller).
        let locked = unsafe { v8::ffi::isolate_is_locked(self) };
        assert!(
            locked,
            "callback_scope!: the isolate is not locked by this thread"
        );
        // SAFETY: the isolate is valid and locked.
        let in_context = unsafe { v8::ffi::isolate_in_context(self) };
        assert!(in_context, "callback_scope!: no context is entered");
        // SAFETY: valid and locked, checked above.
        unsafe { v8::IsolatePtr::from_ffi(self) }
    }
}

#[expect(unsafe_code, reason = "reads the isolate from a raw pointer")]
impl CallbackScopeParam for *mut v8::ffi::FunctionCallbackInfo {
    unsafe fn into_isolate(self) -> v8::IsolatePtr {
        // SAFETY: V8 passes a valid FunctionCallbackInfo with the isolate locked (guaranteed
        // by caller).
        unsafe { v8::IsolatePtr::from_ffi(v8::ffi::fci_get_isolate(self)) }
    }
}

/// Storage of a lock for a `HandleScope` that C++ or V8 opened before calling into Rust.
/// Use [`callback_scope!`] to open one.
pub struct CallbackScope {
    lock: Lock<'static>,
    open: bool,
    _pinned: PhantomPinned,
}

#[expect(unsafe_code, reason = "hands out a lock for a scope opened by C++")]
impl CallbackScope {
    /// Creates the storage of a lock for the innermost `HandleScope` of an isolate.
    ///
    /// # Safety
    /// `param` is either the `FunctionCallbackInfo` V8 passed to the running callback, or a
    /// valid isolate pointer. The isolate must be locked by this thread with a context
    /// entered, and its innermost `v8::HandleScope` must stay open, and stay innermost
    /// whenever no scope opened through the lock is open, until the storage is dropped.
    pub unsafe fn new(param: impl CallbackScopeParam) -> Self {
        Self {
            // SAFETY: forwarded from the caller.
            lock: Lock::new(unsafe { param.into_isolate() }),
            open: false,
            _pinned: PhantomPinned,
        }
    }

    /// Opens the scope and returns its lock.
    ///
    /// # Panics
    /// Panics if a scope of another isolate is open on this thread.
    pub fn open<'s>(self: Pin<&'s mut Self>) -> &'s mut Lock<'s> {
        // SAFETY: nothing is moved out of the storage.
        let this = unsafe { self.get_unchecked_mut() };
        if !this.open {
            scope_owner::enter(this.lock.isolate);
            this.open = true;
        }
        // SAFETY: `Lock<'s>` differs from `Lock<'static>` only in its brand; `new`'s caller
        // keeps the scope open and innermost while the storage lives.
        unsafe { &mut *(&raw mut this.lock).cast::<Lock<'s>>() }
    }
}

impl Drop for CallbackScope {
    fn drop(&mut self) {
        if self.open {
            scope_owner::exit();
        }
    }
}

/// Opens a [`HandleScope`] nested in the scope of a lock and binds its lock to `$lock`.
///
/// The parent lock stays borrowed until the end of the enclosing block, where the scope
/// closes.
///
/// ```ignore
/// jsg::scope!(let lock, lock);
/// ```
#[macro_export]
macro_rules! scope {
    (let $lock:ident, $parent:expr $(,)?) => {
        let mut $lock = ::std::pin::pin!($crate::HandleScope::new($parent));
        let $lock = $lock.as_mut().open();
    };
}

/// Opens an [`EscapableHandleScope`] nested in the scope of a lock and binds its
/// [`EscapableLock`] to `$lock`.
///
/// ```ignore
/// jsg::escapable_handle_scope!(let lock, lock);
/// let obj = lock.new_object();
/// lock.escape(obj)
/// ```
#[macro_export]
macro_rules! escapable_handle_scope {
    (let $lock:ident, $parent:expr $(,)?) => {
        let mut $lock = ::std::pin::pin!($crate::EscapableHandleScope::new($parent));
        let $lock = $lock.as_mut().open();
    };
}

/// Binds to `$lock` a lock for the `HandleScope` that C++ or V8 opened before calling into
/// Rust; see [`CallbackScope::new`] for what the caller must guarantee.
///
/// `$param` is the `*mut FunctionCallbackInfo` of a V8 callback or a `*mut Isolate`.
///
/// ```ignore
/// jsg::callback_scope!(unsafe let lock, isolate);
/// ```
#[macro_export]
macro_rules! callback_scope {
    (unsafe let $lock:ident, $param:expr $(,)?) => {
        // Evaluated outside the `unsafe` block, so `$param` gets no unsafe context.
        let param = $param;
        // SAFETY: the `unsafe` in the invocation passes the obligation to its caller.
        let mut $lock = ::std::pin::pin!(unsafe { $crate::CallbackScope::new(param) });
        let $lock = $lock.as_mut().open();
    };
}
