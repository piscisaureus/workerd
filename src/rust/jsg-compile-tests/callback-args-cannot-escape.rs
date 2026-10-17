// The arguments of a callback live in the callback's scope and cannot be returned from it.
use jsg::v8;
pub fn t(info: *mut v8::ffi::FunctionCallbackInfo) -> v8::Local<'static, v8::Value> {
    // SAFETY: only type-checked.
    jsg::callback_scope!(unsafe let lock, info);
    // SAFETY: only type-checked.
    let args = unsafe { v8::FunctionCallbackInfo::new(lock, info) };
    args.this() //~ ERROR E0515

    //~| ERROR E0515
}
