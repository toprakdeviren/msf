// Assignment-style mismatch — exercises the binary '=' type-check path.
// NOTE: msf currently does NOT check the initializer of `let x: T = expr`
// against T at declaration time, so we use a separate assignment line
// to drive the existing diagnostic.
var s: String = "ok"
s = 42 // expected-error{{Type mismatch: expected 'String', got 'Int'}}

var n: Int = 0
n = "hello" // expected-error{{Type mismatch: expected 'Int', got 'String'}}
