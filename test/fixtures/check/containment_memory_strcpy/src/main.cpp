#include <cstring>

namespace app {

void store_name(char* dst, const char* src) {
    strcpy(dst, src);
    strcat(dst, "_suffix");
}

} // namespace app
