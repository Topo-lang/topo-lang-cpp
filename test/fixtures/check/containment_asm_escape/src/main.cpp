namespace app {

int compute(int x) {
    int result;
    asm("mov %1, %0" : "=r"(result) : "r"(x));
    return result;
}

} // namespace app
