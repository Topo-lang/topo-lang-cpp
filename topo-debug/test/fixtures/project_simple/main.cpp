// Project-level E2E fixture.
//
// Companion to main.topo. The breakpoint on line 6 (`int sentinel = 0;`)
// fires once `data` is fully populated 1..16. The trailing summation forces
// the array to stay live across optimisation; without it -O2 in dev mode
// would constant-fold the printout and lldb would lose `data` to register
// promotion.

#include <cstdio>
int main() {
    int data[16] = {};
    for (int i = 0; i < 16; ++i) data[i] = i + 1;
    int sentinel = 0;  // breakpoint here (line 13)
    (void)sentinel;
    int s = 0;
    for (int i = 0; i < 16; ++i) s += data[i];
    std::printf("data sum=%d\n", s);
    return 0;
}
