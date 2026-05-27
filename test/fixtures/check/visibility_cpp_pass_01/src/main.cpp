// All declared functions are public — any call is legal.

namespace app {

void stepA() {
    // no-op
}

void stepB() {
    stepA();  // public → public, same namespace: allowed
}

void run() {
    stepA();
    stepB();
}

} // namespace app
