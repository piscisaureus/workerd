// Boxing a handle does not let it leave its scope.
use jsg::v8;
pub fn t<'s>(lock: &'s mut jsg::Lock<'s>) -> Box<v8::Local<'s, v8::Object>> {
    jsg::scope!(let scope, lock);
    Box::new(scope.new_object()) //~ ERROR E0515

    //~| ERROR E0515
}
