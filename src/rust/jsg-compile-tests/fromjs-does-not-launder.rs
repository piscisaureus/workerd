// `FromJS` for a handle type returns a handle of the same scope as its input.
use jsg::FromJS;
use jsg::v8;

pub fn t(lock: &mut jsg::Lock<'_>) {
    let keep: Option<v8::Local<'_, v8::Uint8Array>>;
    {
        jsg::scope!(let scope, lock);
        //~^ ERROR E0716
        //~| ERROR E0716
        let value = scope.undefined();
        keep = <v8::Local<'_, v8::Uint8Array> as FromJS<'_>>::from_js(scope, value).ok();
    }
    let _ = keep;
}
