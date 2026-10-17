// A lock stands for holding the isolate lock on this thread; it cannot move to another one.
fn assert_send<T: Send>() {}

pub fn t() {
    assert_send::<jsg::Lock<'static>>(); //~ ERROR E0277

    //~| ERROR E0277
}
