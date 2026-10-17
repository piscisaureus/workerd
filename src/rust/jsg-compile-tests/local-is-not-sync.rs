// Handles are only valid on the isolate's thread.
use jsg::v8;
fn assert_sync<T: Sync>() {}

pub fn t() {
    assert_sync::<v8::Local<'static, v8::Value>>(); //~ ERROR E0277

    //~| ERROR E0277
}
