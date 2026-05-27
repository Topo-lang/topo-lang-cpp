// Non-external function calls exit() — containment violation (process termination).
#include <cstdlib>

namespace app {

int process(int x) {
    if (x < 0) exit(1);
    return x;
}

} // namespace app
