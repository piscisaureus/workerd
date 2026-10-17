// `ToJS` returns a handle of the lock's scope, never a `'static` one.
use jsg::ToJS;
use jsg::v8;

pub fn t(lock: &mut jsg::Lock<'_>) {
    let _: v8::Local<'static, v8::Value> = true.to_js(lock); //~ ERROR lifetime may not live long enough
}
