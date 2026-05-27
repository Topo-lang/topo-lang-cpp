// Would be a violation under `mode = "force"` — an undeclared function
// calls `app::secret` from outside the namespace. With visibility off,
// no errors are produced.

namespace app {

void secret() {
    // private
}

void api() {
    // no-op
}

void run() {
    api();
}

} // namespace app

namespace outside {

void trespass() {
    app::secret();  // would violate — but mode=off
}

} // namespace outside
