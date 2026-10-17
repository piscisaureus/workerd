// Annotations on the same line, the previous line (^) and the line of the previous annotation
// (|); patterns that are a code, message text, or both.

pub fn same_line() -> u32 {
    "not a number" //~ ERROR E0308
}

pub fn previous_line() -> u32 {
    "not a number"
    //~^ ERROR mismatched types
}

pub fn two_on_one_line(_a: Undefined, _b: AlsoUndefined) {} //~ ERROR E0425: `Undefined`
//~| ERROR E0425
