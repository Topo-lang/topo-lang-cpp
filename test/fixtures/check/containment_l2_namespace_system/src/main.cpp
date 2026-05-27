#include <cstdlib>

namespace mylib {
int system(int x) { return x; }  // safe user function
}

namespace app {
int process(int x) {
    mylib::system(x);     // safe — user function
    ::system("echo pwn"); // unsafe — POSIX system()
    return x;
}
}
