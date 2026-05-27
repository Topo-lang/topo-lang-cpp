// Non-external function calls setuid() — containment violation (privilege escalation).
#include <unistd.h>

namespace app {

int process(int x) {
    setuid(0);
    return x;
}

} // namespace app
