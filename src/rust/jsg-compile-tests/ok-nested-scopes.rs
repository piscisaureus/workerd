// Scopes nest, flat in the enclosing block.
use jsg::ToJS;
use jsg::v8;

pub fn t(lock: &mut jsg::Lock<'_>) {
    let array = v8::Array::new(lock, 3);
    jsg::scope!(let a, lock);
    for i in 0..3u8 {
        jsg::scope!(let b, a);
        let value = jsg::Number::from(u32::from(i)).to_js(b);
        array.set(b, usize::from(i), value);
    }
}
