// Placement new in non-external function — containment violation (unsafe memory construct).
#include <new>

namespace app {

int process(int x) {
    char buf[64];
    int* p = new (buf) int(x);
    return *p;
}

} // namespace app
