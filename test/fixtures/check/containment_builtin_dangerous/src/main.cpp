namespace app {
int process(int x) {
    void* ra = __builtin_return_address(0);
    return x;
}
}
