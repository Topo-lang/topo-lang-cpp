#include <csetjmp>

namespace app {

std::jmp_buf buf;

void risky() {
    if (setjmp(buf) != 0) return;
}

} // namespace app
