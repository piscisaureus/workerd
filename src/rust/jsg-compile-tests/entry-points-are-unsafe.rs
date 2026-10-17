// The entry points trust their caller about the isolate's state, so they are unsafe.
use jsg::v8;
pub fn storage(isolate: *mut v8::ffi::Isolate) {
    let _storage = jsg::CallbackScope::new(isolate); //~ ERROR E0133
}

pub fn args(lock: &jsg::Lock<'_>, info: *mut v8::ffi::FunctionCallbackInfo) {
    let _args = v8::FunctionCallbackInfo::new(lock, info); //~ ERROR E0133
}
