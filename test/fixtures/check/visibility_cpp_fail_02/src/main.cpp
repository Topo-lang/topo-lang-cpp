// Visibility violation: an undeclared function `attacker` calls
// `core::initInternal` — internal functions must only be referenced from
// declared (in .topo) functions.

namespace core {

void initInternal() {
    // internal implementation
}

void bootstrap() {
    initInternal();  // declared caller — OK
}

void run() {
    bootstrap();
}

} // namespace core

// `attacker` is NOT declared in .topo — calling an `internal` function
// from it is a visibility violation.
void attacker() {
    core::initInternal();  // violation: internal called from external
}
