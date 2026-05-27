// Struct-of-primitives extract fixture.
//
// Exercises the v2 LayoutDescriptor adapter branch (struct emission) on a
// 4-element array of a trivial aggregate. Compiled at -O2 -g via the
// CMakeLists rule below, with explicit field reads through `volatile keep`
// so DWARF retains the field offsets and the array is not dead-stripped.
#include <cstdio>
struct Particle { float x; float y; float z; };
int main() {
    Particle p[4] = {{1,2,3},{4,5,6},{7,8,9},{10,11,12}};
    // Reference all fields so -O2 keeps them.
    volatile float keep = p[0].x + p[0].y + p[0].z
                        + p[1].x + p[1].y + p[1].z
                        + p[2].x + p[2].y + p[2].z
                        + p[3].x + p[3].y + p[3].z;
    int sentinel = 0;  // breakpoint here — adapter reads `p` at this line
    (void)sentinel;
    (void)keep;
    std::puts("done");
    return 0;
}
