struct Box { var v: Int }
let b = Box(v: 1)
b.v = 2 // expected-error{{Cannot assign to property: 'b' is a 'let' constant}}
