// Tests that user-namespace-qualified calls and member-access calls
// don't trigger L1 false positives.

namespace utils {
    int compute(int x) { return x * 2; }
}

namespace app {

struct Processor {
    int value;
    int compute(int x) const { return value + x; }
};

int process(int x) {
    Processor p{10};
    // Member access — should NOT trigger containment
    int a = p.compute(x);
    // Namespace-qualified call — should NOT trigger containment
    int b = utils::compute(x);
    return a + b;
}

} // namespace app
