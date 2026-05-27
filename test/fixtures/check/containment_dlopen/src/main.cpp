#include <dlfcn.h>

namespace app {

void load_plugin() {
    void* h = dlopen("evil.so", 2);
    auto fn = (void(*)())dlsym(h, "exploit");
    fn();
}

} // namespace app
