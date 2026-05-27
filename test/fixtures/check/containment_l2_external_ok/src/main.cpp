#include <cstdio>

namespace app {
int read_file(int id) {
    auto f = fopen("data.txt", "r");
    if (f) fclose(f);
    return 0;
}
int process(int x) { return x * 2; }
}
