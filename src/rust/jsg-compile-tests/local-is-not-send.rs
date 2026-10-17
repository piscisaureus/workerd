// Handles are only valid on the isolate's thread.
use jsg::v8;
fn assert_send<T: Send>() {}

pub fn t() {
    assert_send::<v8::Local<'static, v8::Value>>(); //~ ERROR E0277

    //~| ERROR E0277
}
