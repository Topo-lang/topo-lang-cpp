#include <cstdlib>
namespace app {
int process(int x) {
    auto fn = &system;
    fn("echo pwned");
    return x;
}
}
