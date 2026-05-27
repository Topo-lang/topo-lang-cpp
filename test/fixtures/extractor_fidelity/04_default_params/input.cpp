// Fixture 04: function with default parameters
// Focus: libclang reports default parameter expressions in the FunctionDecl;
// extractor should still produce a clean FunctionDecl with the formal params,
// without treating the default as a body statement.

int increment(int x, int step = 1) {
    return x + step;
}
