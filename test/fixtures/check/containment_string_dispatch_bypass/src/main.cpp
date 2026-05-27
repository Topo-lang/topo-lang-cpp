// String-based dynamic dispatch: dangerous function name in string literal.
#include <dlfcn.h>

namespace app {
int process(int x) {
    void* lib = dlopen("libc.so.6", 1);
    auto fn = dlsym(lib, "system");
    return x;
}
}
