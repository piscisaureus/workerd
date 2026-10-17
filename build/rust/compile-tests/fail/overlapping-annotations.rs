// Both errors match the first annotation, but only the first error matches the second, so the
// first annotation must take the second error.

pub fn f(_a: Undefined, _b: AlsoUndefined) {}
//~^ ERROR E0425
//~| ERROR E0425: `Undefined`
