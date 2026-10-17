// Rust code called from C++ returns a handle of the caller's scope.
use jsg::v8;
pub fn t(isolate: *mut v8::ffi::Isolate) -> v8::ffi::Local {
    // SAFETY: only type-checked.
    jsg::callback_scope!(unsafe let lock, isolate);
    let object: v8::Local<'_, v8::Value> = lock.new_object().into();
    // SAFETY: only type-checked.
    unsafe { object.into_ffi() }
}
