// Visibility violation: `other::invoke` calls the private `app::helper`,
// which crosses a namespace boundary.

namespace app {

void helper() {
    // private implementation detail
}

void compute() {
    helper();  // same-namespace private call — OK
}

void run() {
    compute();
}

} // namespace app

namespace other {

void invoke() {
    app::helper();  // violation: cross-namespace private call
}

} // namespace other
