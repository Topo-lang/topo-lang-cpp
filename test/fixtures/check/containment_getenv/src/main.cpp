#include <cstdlib>
namespace app {
int process(int x) {
    const char* secret = getenv("SECRET_KEY");
    return x;
}
}
