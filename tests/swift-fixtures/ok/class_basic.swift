// expected-no-diagnostics

class Counter {
    var value: Int

    init(start: Int) {
        self.value = start
    }

    func bump() {
        self.value = self.value + 1
    }
}

let c = Counter(start: 0)
