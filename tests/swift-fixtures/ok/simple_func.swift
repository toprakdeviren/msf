// expected-no-diagnostics

func add(_ a: Int, _ b: Int) -> Int {
    return a + b
}

func greet(name: String) -> String {
    return "Hello, " + name
}

let s = add(1, 2)
let g = greet(name: "world")
