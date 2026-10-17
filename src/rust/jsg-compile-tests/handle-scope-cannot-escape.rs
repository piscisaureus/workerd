// A nested scope's lock cannot outlive the scope.
pub fn t(lock: &mut jsg::Lock<'_>) {
    let escaped;
    {
        jsg::scope!(let scope, lock);
        //~^ ERROR E0716
        //~| ERROR E0716
        escaped = scope;
    }
    let _ = escaped.new_object();
}
