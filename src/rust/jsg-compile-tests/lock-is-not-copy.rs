// A lock cannot be duplicated.
pub fn t(lock: &mut jsg::Lock<'_>) {
    let _copy = *lock; //~ ERROR E0507
}
