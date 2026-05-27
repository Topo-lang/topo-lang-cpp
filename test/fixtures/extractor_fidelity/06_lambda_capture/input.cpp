// Fixture 06: function body containing a lambda expression with captures
// Focus: LambdaExpr with capture-by-value and capture-by-reference.
// Exercises the extractor's libclang lambda handling and stmt body recursion.

int use_lambda(int x) {
    int y = 10;
    auto add = [x, &y](int z) {
        y = y + z;
        return x + z;
    };
    return add(5);
}
