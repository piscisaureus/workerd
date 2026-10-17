// Opening a `Global` needs a lock, and the handle it returns is tied to that lock's scope.
use jsg::v8;
pub fn t<'s>(
    lock: &'s mut jsg::Lock<'s>,
    global: &v8::Global<v8::Object>,
) -> v8::Local<'s, v8::Object> {
    jsg::scope!(let scope, lock);
    global.as_local(scope) //~ ERROR E0515

    //~| ERROR E0515
}
