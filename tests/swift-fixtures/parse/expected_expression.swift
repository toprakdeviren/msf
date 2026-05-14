// `+` followed by another operator triggers the parser's
// "expected expression after operator" diagnostic.
let x = 1 + * 2 // expected-error{{expected expression after operator}}
