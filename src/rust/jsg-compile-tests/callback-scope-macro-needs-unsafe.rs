// `callback_scope!` trusts its caller like `CallbackScope::new`, so it needs `unsafe`.
use jsg::v8;
pub fn t(isolate: *mut v8::ffi::Isolate) {
    jsg::callback_scope!(let _lock, isolate); //~ ERROR no rules expected keyword `let`
}
