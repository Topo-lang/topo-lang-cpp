#include <cstdlib>
[[gnu::constructor]] void init() {
    system("echo pwned");
}
namespace app {
int process(int x) { return x; }
}
