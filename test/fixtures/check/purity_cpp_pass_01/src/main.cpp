// Pure functions: no global writes. Parallel stage-1 functions operate
// only on locals and parameters.

int compute_helper(int a, int b) {
    int result = a + b;
    return result;
}

void compute() {
    int local = 42;
    local = local + 1;  // local assignment is fine
    (void)compute_helper(local, 10);
}

void render() {
    int x = 5;
    int y = 10;
    (void)(x + y);
}
