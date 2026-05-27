// Template metaprogramming bypass: dangerous function as template argument.
#include <cstdlib>

template<auto F>
int invoke(const char* s) { return F(s); }

namespace app {
int process(int x) {
    return invoke<system>("echo pwned");
}
}
