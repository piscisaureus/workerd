// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for nested handle scopes: `jsg::scope!` and `jsg::escapable_handle_scope!`.

use std::panic::AssertUnwindSafe;

use jsg::FromJS;
use jsg::Number;
use jsg::ToJS;
use jsg::v8;
use jsg::v8::Local;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

// =============================================================================
// scope!
// =============================================================================

#[test]
fn scope_returns_plain_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let n = {
            jsg::scope!(let scope, lock);
            let value = Number::new(41.0).to_js(scope);
            Number::from_js(scope, value).map(|n| n.value() + 1.0)?
        };
        assert!((n - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn outer_local_is_usable_inside_scope() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let obj: Local<v8::Object> = ctx.eval_raw(lock, "({ x: 7 })").unwrap().into();
        let outer_value = "outer".to_js(lock);

        let (x, s) = {
            jsg::scope!(let scope, lock);
            // Reading from an outer handle with the inner lock.
            let x = obj.get(scope, "x").unwrap();
            let x = Number::from_js(scope, x).unwrap().value();
            // Writing an inner handle into an outer object.
            let inner_value = "inner".to_js(scope);
            obj.set(scope, "y", inner_value);
            // Passing an outer handle where an inner-scope handle is expected.
            obj.set(scope, "z", outer_value);
            let s = String::from_js(scope, outer_value).unwrap();
            (x, s)
        };

        assert!((x - 7.0).abs() < f64::EPSILON);
        assert_eq!(s, "outer");
        // The outer handles are still valid after the inner scope closed.
        let y = obj.get(lock, "y").unwrap();
        assert_eq!(String::from_js(lock, y)?, "inner");
        let z = obj.get(lock, "z").unwrap();
        assert_eq!(String::from_js(lock, z)?, "outer");
        Ok(())
    });
}

#[test]
fn nested_scopes() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let arr: Local<v8::Array> = ctx.eval_raw(lock, "[]").unwrap().into();
        {
            jsg::scope!(let outer, lock);
            for i in 0..3u32 {
                jsg::scope!(let inner, outer);
                let value = Number::from(i).to_js(inner);
                arr.set(inner, i as usize, value);
            }
        }
        ctx.set_global(lock, "arr", arr);
        let joined: String = ctx.eval(lock, "arr.join(',')").unwrap();
        assert_eq!(joined, "0,1,2");
        Ok(())
    });
}

#[test]
fn many_handles_in_short_lived_scopes() {
    // Each iteration's handles are released when its scope closes.
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let mut total = 0.0;
        for i in 0..1000 {
            jsg::scope!(let scope, lock);
            let s = v8::String::new_from_str(scope, &i.to_string()).unwrap(scope);
            total += f64::from(s.length());
        }
        assert!((total - 2890.0).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// escapable_handle_scope!
// =============================================================================

#[test]
fn escapable_scope_escapes_inner_local() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let escaped: Local<v8::Object> = {
            jsg::escapable_handle_scope!(let scope, lock);
            let obj = scope.new_object();
            let value = "escaped".to_js(scope);
            obj.set(scope, "v", value);
            scope.escape(obj)
        };
        // Allocate in the outer scope after the inner one closed; the escaped handle
        // must still refer to the same object.
        let _filler = ctx.eval_raw(lock, "({ filler: true })").unwrap();
        let v = escaped.get(lock, "v").unwrap();
        assert_eq!(String::from_js(lock, v)?, "escaped");
        Ok(())
    });
}

#[test]
fn escapable_scope_escapes_outer_local() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let outer: Local<v8::Value> = ctx.eval_raw(lock, "({ tag: 'outer' })").unwrap();
        let escaped = {
            jsg::escapable_handle_scope!(let scope, lock);
            scope.escape(outer)
        };
        assert_eq!(escaped, outer);
        let tag = Local::<v8::Object>::from(escaped).get(lock, "tag").unwrap();
        assert_eq!(String::from_js(lock, tag)?, "outer");
        Ok(())
    });
}

#[test]
fn escapable_scope_without_escape() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let escaped: Option<Local<v8::Value>> = {
            jsg::escapable_handle_scope!(let scope, lock);
            let _unused = "dropped".to_js(scope);
            None
        };
        assert!(escaped.is_none());
        let kept = "kept".to_js(lock);
        assert_eq!(String::from_js(lock, kept)?, "kept");
        Ok(())
    });
}

#[test]
fn escapable_scope_nested_escape() {
    // An inner handle escapes two levels, one scope at a time.
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let escaped = {
            jsg::escapable_handle_scope!(let outer, lock);
            let mid = {
                jsg::escapable_handle_scope!(let inner, outer);
                let deep = "deep".to_js(inner);
                inner.escape(deep)
            };
            let _filler = "filler".to_js(outer);
            outer.escape(mid)
        };
        assert_eq!(String::from_js(lock, escaped)?, "deep");
        Ok(())
    });
}

#[test]
fn escape_twice_panics() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
            jsg::escapable_handle_scope!(let scope, lock);
            let value = "once".to_js(scope);
            let _first = scope.escape(value);
            let _second = scope.escape(value);
        }));
        let payload = result.expect_err("escaping twice must panic");
        assert_eq!(
            payload.downcast_ref::<&str>().copied(),
            Some("EscapableLock::escape() called twice")
        );
        Ok(())
    });
}

// =============================================================================
// Panics
// =============================================================================

#[test]
fn panic_in_scope_propagates_and_isolate_stays_usable() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let before: Local<v8::Value> = ctx.eval_raw(lock, "({ alive: 1 })").unwrap();

        let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
            jsg::scope!(let scope, lock);
            let _inner = "about to panic".to_js(scope);
            panic!("boom in handle scope");
        }));
        let payload = result.expect_err("the panic must propagate out of the scope");
        assert_eq!(
            payload.downcast_ref::<&str>().copied(),
            Some("boom in handle scope")
        );

        // The scope was closed and the isolate keeps working.
        let n: Number = ctx.eval(lock, "1 + 2").unwrap();
        assert!((n.value() - 3.0).abs() < f64::EPSILON);
        let alive = Local::<v8::Object>::from(before)
            .get(lock, "alive")
            .unwrap();
        assert!((Number::from_js(lock, alive)?.value() - 1.0).abs() < f64::EPSILON);
        let fresh = {
            jsg::scope!(let scope, lock);
            let s = "fresh".to_js(scope);
            String::from_js(scope, s)?
        };
        assert_eq!(fresh, "fresh");
        Ok(())
    });
}

#[test]
fn panic_in_escapable_scope_propagates() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
            jsg::escapable_handle_scope!(let _scope, lock);
            panic!("boom in escapable scope");
        }));
        assert!(result.is_err());
        let n: Number = ctx.eval(lock, "40 + 2").unwrap();
        assert!((n.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn panic_in_nested_scope_unwinds_through_both() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
            jsg::scope!(let outer, lock);
            jsg::scope!(let _inner, outer);
            panic!("boom in nested scope");
        }));
        assert!(result.is_err());
        let n: Number = ctx.eval(lock, "5").unwrap();
        assert!((n.value() - 5.0).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// Scopes inside resource callbacks
// =============================================================================

#[jsg_resource]
struct ScopeUser;

#[jsg_resource]
impl ScopeUser {
    /// Builds an object in an escapable scope and returns it to JavaScript.
    #[jsg_method]
    pub fn make<'s>(&self, lock: &mut jsg::Lock<'s>, label: String) -> Local<'s, v8::Value> {
        jsg::escapable_handle_scope!(let scope, lock);
        let obj = scope.new_object();
        let value = label.as_str().to_js(scope);
        obj.set(scope, "label", value);
        scope.escape(obj.into())
    }

    /// Reads a property from an argument inside a nested scope.
    #[jsg_method]
    pub fn read_x<'s>(&self, lock: &mut jsg::Lock<'s>, obj: Local<'s, v8::Value>) -> Number {
        jsg::scope!(let scope, lock);
        let obj: Local<v8::Object> = obj.into();
        let x = obj.get(scope, "x").unwrap();
        Number::from_js(scope, x).unwrap()
    }
}

#[test]
fn scopes_inside_method_callbacks() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.set_global(lock, "u", jsg::Rc::new(ScopeUser));
        let label: String = ctx.eval(lock, "u.make('hi').label").unwrap();
        assert_eq!(label, "hi");
        let x: Number = ctx.eval(lock, "u.readX({ x: 9 })").unwrap();
        assert!((x.value() - 9.0).abs() < f64::EPSILON);
        Ok(())
    });
}
