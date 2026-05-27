#include <vector>
#include <algorithm>

namespace app {

int compute(int x, int y) {
    return x + y;
}

int sum(int x) {
    std::vector<int> v = {1, 2, 3, x};
    int total = 0;
    for (auto n : v) total += n;
    return total;
}

} // namespace app
