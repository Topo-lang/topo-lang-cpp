// Project-level multi-variable summary
// fixture. Companion to main.topo. Two arrays in the same frame so the
// summary template references both via `{sum(a)}` / `{sum(b)}` /
// `{sum(a) + sum(b)}` — all resolved by a *single* adapter spawn via
// the multi-var protocol with summary batching.
//
// Both arrays are filled via runtime loops (not constant initialisers) so
// the optimiser can't register-promote them out of stack memory before the
// breakpoint. The trailing summation keeps both alive past the breakpoint.

#include <cstdio>
int main() {
    int a[4] = {};
    for (int i = 0; i < 4; ++i) a[i] = i + 1;         // a = {1,2,3,4}  sum=10
    int b[4] = {};
    for (int i = 0; i < 4; ++i) b[i] = (i + 1) * 10;  // b = {10..40}   sum=100
    int sentinel = 0;  // breakpoint here (line 17)
    (void)sentinel;
    int t = 0;
    for (int i = 0; i < 4; ++i) t += a[i] + b[i];
    std::printf("a+b sum=%d\n", t);
    return 0;
}
