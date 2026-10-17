// A handle created in a nested scope cannot be stored in a variable that outlives it.
pub fn t(lock: &mut jsg::Lock<'_>) {
    let mut out = None;
    {
        jsg::scope!(let scope, lock);
        //~^ ERROR E0716
        //~| ERROR E0716
        out = Some(scope.new_object());
    }
    let _ = out;
}
