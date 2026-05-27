namespace app {
int process(int x) {
    long addr = 0x1234;
    int* p = reinterpret_cast
        <int*>(addr);
    return *p;
}
}
