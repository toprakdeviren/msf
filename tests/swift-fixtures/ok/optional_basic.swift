// expected-no-diagnostics

let maybe: Int? = 42

if let x = maybe {
    let y = x + 1
}

let arr: [Int] = [1, 2, 3]
let first = arr[0]
