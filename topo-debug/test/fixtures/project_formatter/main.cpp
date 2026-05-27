// Companion to main.topo.
//
// `Mesh` is a struct with two split column arrays (the shape a SoA
// data-layout transform leaves behind). The breakpoint on the `int
// sentinel = 0;` line fires once both columns are populated. The
// trailing summation keeps the struct live across the breakpoint so
// -O2 dev-mode optimisation cannot register-promote it away (same
// trick project_simple/main.cpp uses).
//
// Without the Topo lldb formatter, `frame variable mesh` dumps the raw
// post-transform layout (the two split arrays). With it, the summary
// template renders the logical declared view: `<Mesh: 6 verts, 9 tris>`.

#include <cstdio>

struct Mesh {
    int verts[6];
    int tris[9];
};

int main() {
    Mesh mesh = {};
    for (int i = 0; i < 6; ++i) mesh.verts[i] = i + 1;
    for (int i = 0; i < 9; ++i) mesh.tris[i] = (i + 1) * 10;
    int sentinel = 0;  // breakpoint here (line 25)
    (void)sentinel;
    long s = 0;
    for (int i = 0; i < 6; ++i) s += mesh.verts[i];
    for (int i = 0; i < 9; ++i) s += mesh.tris[i];
    std::printf("mesh checksum=%ld\n", s);
    return 0;
}
