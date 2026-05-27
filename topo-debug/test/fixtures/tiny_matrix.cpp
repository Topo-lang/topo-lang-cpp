// Tiny lldb extract fixture.
#include <cstdio>
int main() {
    int matrix[16][16] = {};
    matrix[5][7] = 42;
    int sentinel = 0;  // breakpoint here — adapter reads matrix at this line
    (void)sentinel;
    std::puts("done");
    return 0;
}
