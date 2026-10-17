// While a nested scope is open, the parent lock is mutably borrowed: new handles must go
// through the nested scope.
pub fn t(lock: &mut jsg::Lock<'_>) {
    jsg::scope!(let scope, lock);
    let _outer = lock.new_object(); //~ ERROR E0499
    let _inner = scope.new_object();
}
