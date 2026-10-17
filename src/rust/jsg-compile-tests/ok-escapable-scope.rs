// An escapable scope escapes an inner or an outer handle into the scope it was opened from.
use jsg::v8;

pub fn make<'s>(lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Object> {
    jsg::escapable_handle_scope!(let scope, lock);
    let object = scope.new_object();
    scope.escape(object)
}

pub fn t<'s>(lock: &mut jsg::Lock<'s>) {
    let outer = lock.new_object();
    let inner = make(lock);
    let same = {
        jsg::escapable_handle_scope!(let scope, lock);
        scope.escape(outer)
    };
    let maybe: Option<v8::Local<'s, v8::Object>> = {
        jsg::escapable_handle_scope!(let scope, lock);
        let object = scope.new_object();
        Some(object).map(|object| scope.escape(object))
    };
    let nested = {
        jsg::escapable_handle_scope!(let a, lock);
        let mid = {
            jsg::escapable_handle_scope!(let b, a);
            let object = b.new_object();
            b.escape(object)
        };
        a.escape(mid)
    };
    inner.set(lock, "same", same.into());
    let _ = (maybe, nested);
}
