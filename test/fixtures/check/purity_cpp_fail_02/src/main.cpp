// Parallel stage function `stepA` writes to an `extern` declared global.
// The extractor must detect extern globals as file-scope.

extern int sharedState;
int sharedState = 0;  // definition also at TU scope

void stepA() {
    sharedState = 42;  // write to extern global — violation
}

void stepB() {
    int local = 1;
    (void)local;
}
