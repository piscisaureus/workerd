// A handle cannot be stored anywhere that outlives its scope, such as a thread-local.
use jsg::v8;
thread_local! {
    static STASH: std::cell::Cell<Option<v8::Local<'static, v8::Object>>> =
        const { std::cell::Cell::new(None) };
}

pub fn t(lock: &mut jsg::Lock<'_>) {
    let object = lock.new_object();
    STASH.with(|cell| cell.set(Some(object))); //~ ERROR E0521
}
