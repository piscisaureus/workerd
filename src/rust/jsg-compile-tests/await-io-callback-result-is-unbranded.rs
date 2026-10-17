// `await_io`'s continuation gets a fresh lock; its result cannot hold that lock's handles.
pub fn t(lock: &mut jsg::Lock<'_>) {
    let _ = lock.await_io(async {}, |lock, ()| Ok(lock.new_object())); //~ ERROR lifetime may not live long enough
}
