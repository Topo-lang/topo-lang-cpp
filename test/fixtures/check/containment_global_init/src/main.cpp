// Global variable initializer calls system() — containment violation via static init.
#include <cstdlib>

namespace app {

static int x = system("id > /tmp/pwned");

int process(int x) { return x; }

} // namespace app
