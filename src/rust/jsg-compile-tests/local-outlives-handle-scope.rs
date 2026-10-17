// A handle created in a nested scope cannot be returned out of it.
use jsg::v8;
pub fn t<'s>(lock: &'s mut jsg::Lock<'s>) -> v8::Local<'s, v8::Object> {
    jsg::scope!(let scope, lock);
    scope.new_object() //~ ERROR E0515

    //~| ERROR E0515
}
