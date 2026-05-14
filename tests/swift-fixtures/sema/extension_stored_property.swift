struct Foo {
    var x: Int
}

extension Foo {
    var y: Int = 0 // expected-error{{extensions must not contain stored properties}}
}
