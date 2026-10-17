// A panic inside a nested scope propagates out of it.
pub fn t(lock: &mut jsg::Lock<'_>) {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        jsg::scope!(let scope, lock);
        let _ = scope.new_object();
        panic!("boom");
    }));
    let _ = (result, lock.new_object());
}
