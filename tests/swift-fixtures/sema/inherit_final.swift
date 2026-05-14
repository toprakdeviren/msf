final class Base {
    var x: Int = 0
}

class Derived: Base {} // expected-error{{cannot inherit from final class 'Base'}}
