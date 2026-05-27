#include <cstdlib>

namespace app {

int process_data(int n) {
    int* buf = static_cast<int*>(malloc(sizeof(int) * n));
    for (int i = 0; i < n; ++i) buf[i] = i;
    int sum = 0;
    for (int i = 0; i < n; ++i) sum += buf[i];
    free(buf);
    return sum;
}

} // namespace app
