// All parallel stage-1 functions are pure: they only read/write locals.
// No file-scope globals are declared, so no writes can be attributed.

int helper_double(int x) {
    int result = x * 2;
    return result;
}

void transform() {
    int tmp = 5;
    tmp = helper_double(tmp);
    (void)tmp;
}

void validate() {
    int a = 1;
    int b = 2;
    int sum = a + b;
    (void)sum;
}
