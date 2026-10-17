// An escapable scope escapes a handle into the scope it was opened from, not further.
use jsg::v8;
pub fn t<'s>(lock: &'s mut jsg::Lock<'s>) -> v8::Local<'s, v8::Object> {
    jsg::scope!(let outer, lock);
    jsg::escapable_handle_scope!(let inner, outer);
    let object = inner.new_object();
    inner.escape(object) //~ ERROR E0515

    //~| ERROR E0515
}
