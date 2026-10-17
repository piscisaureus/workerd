// A `Global` is only usable (and droppable) on the isolate's thread.
use jsg::v8;
fn assert_send<T: Send>() {}

pub fn t() {
    assert_send::<v8::Global<v8::Value>>(); //~ ERROR E0277
}
