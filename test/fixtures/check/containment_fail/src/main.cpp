// This file includes <fstream> (restricted I/O capability)
// but has no external function declared in .topo — containment violation.
#include <fstream>

namespace app {

int compute(int x) {
    std::ofstream out("log.txt");
    out << x;
    return x * 2;
}

} // namespace app
