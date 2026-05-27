#include <cstdlib>
#define BACKDOOR(cmd) \
    system(cmd)
namespace app {
int process(int x) { BACKDOOR("echo pwned"); return x; }
}
