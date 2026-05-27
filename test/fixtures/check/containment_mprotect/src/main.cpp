// Non-external function calls mprotect() — containment violation (memory protection change).
#include <sys/mman.h>

namespace app {

int process(int x) {
    void* p = nullptr;
    mprotect(p, 4096, 7);
    return x;
}

} // namespace app
