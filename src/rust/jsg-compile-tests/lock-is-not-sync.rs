// A lock cannot be shared with another thread.
fn assert_sync<T: Sync>() {}

pub fn t() {
    assert_sync::<jsg::Lock<'static>>(); //~ ERROR E0277

    //~| ERROR E0277
}
