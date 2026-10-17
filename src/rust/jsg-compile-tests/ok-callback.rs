// A callback reads its arguments and sets its return value in its own scope.
use jsg::v8;
pub fn t(info: *mut v8::ffi::FunctionCallbackInfo) {
    // SAFETY: only type-checked.
    jsg::callback_scope!(unsafe let lock, info);
    // SAFETY: only type-checked.
    let mut args = unsafe { v8::FunctionCallbackInfo::new(lock, info) };
    let object = lock.new_object();
    object.set(lock, "this", args.this());
    args.set_return_value(object.into());
}
