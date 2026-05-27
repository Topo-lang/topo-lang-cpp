// Companion to main.topo.
//
// Stack-allocates an 8-element int array, populates it via a runtime
// loop so dev-mode optimisation cannot constant-fold the storage away
// (a plain `int data[8] = {1,2,3,...}` aggregate init gets eliminated
// under -O2). xs half (data[0..4]) = {1, 2, 3, 4}; ys half
// (data[4..8]) = {10, 20, 30, 40}. The trailing summation keeps the
// array live across the breakpoint, matching the trick used by
// project_simple/main.cpp.

#include <cstdio>
int main() {
    int data[8] = {};
    for (int i = 0; i < 4; ++i) data[i] = i + 1;
    for (int i = 0; i < 4; ++i) data[i + 4] = (i + 1) * 10;
    int sentinel = 0;  // breakpoint here (line 14)
    (void)sentinel;
    int s = 0;
    for (int i = 0; i < 8; ++i) s += data[i];
    std::printf("data sum=%d\n", s);
    return 0;
}
