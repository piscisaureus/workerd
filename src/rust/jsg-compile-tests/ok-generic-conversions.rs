// Code generic over the scope converts in both directions.
use jsg::FromJS;
use jsg::ToJS;

pub fn round_trip<'s, T>(lock: &mut jsg::Lock<'s>, value: T) -> Result<T, jsg::Error>
where
    T: ToJS<'s> + FromJS<'s, ResultType = T>,
{
    let js = value.to_js(lock);
    T::from_js(lock, js)
}

pub fn owned<T: for<'s> FromJS<'s, ResultType = T>>() {}

pub fn t() {
    owned::<String>();
    owned::<Vec<jsg::Number>>();
}
