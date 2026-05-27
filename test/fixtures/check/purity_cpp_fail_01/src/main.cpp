// Parallel stage function `compute` writes to a file-scope static global.
// Expected: purity violation for `compute` in parallel stage<1>.
//
// C++ functions are at global scope (no namespace wrapper) so the extractor
// emits simple names that match the .topo calledFunctions entries.

static int counter = 0;

void compute() {
    counter = counter + 1;
}

void render() {
    // pure: no global access
}
