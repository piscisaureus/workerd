// A lock for the caller's scope cannot outlive its storage, nor can its handles.
use jsg::v8;
pub fn t(isolate: *mut v8::ffi::Isolate) -> v8::Local<'static, v8::Object> {
    // SAFETY: only type-checked.
    jsg::callback_scope!(unsafe let lock, isolate);
    lock.new_object() //~ ERROR E0515

    //~| ERROR E0515
}
