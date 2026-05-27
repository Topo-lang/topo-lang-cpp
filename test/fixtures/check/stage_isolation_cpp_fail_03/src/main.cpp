// Both `loadA` and `loadB` (stage<1>) call `merge` (stage<2>) — two
// forward stage violations in the same fn block.

void merge() {
    // stage 2
}

void loadA() {
    merge();  // violation #1: stage 1 → stage 2
}

void loadB() {
    merge();  // violation #2: stage 1 → stage 2
}

void run() {
    loadA();
    loadB();
    merge();
}
