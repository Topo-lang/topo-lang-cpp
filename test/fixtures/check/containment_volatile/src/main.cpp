namespace app {

void poll_hardware() {
    volatile int* reg = (volatile int*)0x40000000;
    while (*reg == 0) {}
}

} // namespace app
