// Parallel `tick()` uses pre-increment `++ticks` on a global — a write,
// violating purity. `monitor()` uses post-increment `ticks++` — also a
// write.

static int ticks = 0;

void tick() {
    ++ticks;
}

void monitor() {
    ticks++;
}
