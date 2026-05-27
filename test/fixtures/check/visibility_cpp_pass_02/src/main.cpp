// `engine::coordinate` (public) calls `engine::detail` (private) from the
// same namespace — visibility check must allow this.

namespace engine {

void detail() {
    // private implementation
}

void coordinate() {
    detail();  // same-namespace private call — OK
}

void run() {
    coordinate();
}

} // namespace engine
