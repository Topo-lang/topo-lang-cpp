#include <cstdlib>
#include <cstring>

namespace app {

// Declared external in topo — allowed to use raw memory APIs.
void* allocator_alloc(unsigned long n) {
    return malloc(n);
}

void allocator_free(void* p) {
    free(p);
}

void buffer_copy(char* dst, const char* src, unsigned n) {
    memcpy(dst, src, n);
}

} // namespace app
