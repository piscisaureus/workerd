// Handles of an outer scope are usable inside a nested one, in both directions.
use jsg::ToJS;

pub fn t(lock: &mut jsg::Lock<'_>) {
    let object = lock.new_object();
    let outer = "outer".to_js(lock);
    {
        jsg::scope!(let scope, lock);
        let inner = "inner".to_js(scope);
        object.set(scope, "inner", inner);
        object.set(scope, "outer", outer);
        let _ = object.get(scope, "inner");
    }
    let _ = object.get(lock, "inner");
}
