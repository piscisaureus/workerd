// A builtin module's callback returns a handle of the scope it is given.
use jsg::ToJS;
use jsg::v8;

pub fn register(registry: std::pin::Pin<&mut v8::ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "node-internal:example",
        |lock| vec![jsg::Number::new(1.0)].to_js(lock),
        jsg::modules::ModuleType::Internal,
    );
}
