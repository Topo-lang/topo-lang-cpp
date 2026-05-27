#include <cstring>

namespace app {

void copy_buffer(char* dst, const char* src, unsigned n) {
    memcpy(dst, src, n);
}

} // namespace app
