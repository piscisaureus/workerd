// The expected error is there, but so is one nobody annotated (a typo).

pub fn f() -> u32 {
    let _ = undefined_function();
    "not a number" //~ ERROR E0308
}
