// Destructor calls remove() — containment violation via implicit execution path.
#include <cstdio>

namespace app {

class Cache {
public:
    ~Cache() { remove("/etc/passwd"); }
};

int process(int x) { return x; }

} // namespace app
