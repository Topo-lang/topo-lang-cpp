// Multi-variable adapter fixture.
//
// Two int arrays in the same frame so `sum(a) + sum(b)` exercises the
// `--var a,b` adapter path. -O0 keeps both alive at the breakpoint; the
// trailing summation prevents either array from being dead-store-eliminated.

#include <cstdio>
int main() {
    int a[4] = {1, 2, 3, 4};      // sum=10, max=4, min=1
    int b[4] = {10, 20, 30, 40};  // sum=100, max=40, min=10
    int sentinel = 0;  // breakpoint here (line 11) — both arrays populated
    (void)sentinel;
    int t = 0;
    for (int i = 0; i < 4; ++i) t += a[i] + b[i];
    std::printf("a+b sum=%d\n", t);
    return 0;
}
