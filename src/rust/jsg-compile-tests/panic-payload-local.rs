// A handle cannot leave its scope as a panic payload.
pub fn t(lock: &mut jsg::Lock<'_>) {
    let object = lock.new_object();
    let _payload: Box<dyn std::any::Any + Send> = Box::new(object);
    //~^ ERROR E0277
    //~| ERROR E0277
}
