// Second lldb extract fixture; exercises the `--var`
// flag with a non-`matrix` variable name and a different leaf dtype (f64)
// so the e2e suite covers both the rename plumbing and a separate type.
#include <cstdio>
int main() {
    double vec[8] = {0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5};
    int sentinel = 0;  // breakpoint here — adapter reads `vec` at this line
    (void)sentinel;
    (void)vec;
    std::puts("done");
    return 0;
}
