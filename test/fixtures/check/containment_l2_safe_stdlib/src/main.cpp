#include <vector>
#include <algorithm>
#include <string>

namespace app {
int process(int x) {
    std::vector<int> v = {1, 2, 3, x};
    std::sort(v.begin(), v.end());
    std::string s = std::to_string(v.front());
    return static_cast<int>(s.size());
}
}
