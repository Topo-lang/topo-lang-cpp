// Fixture 08: function body contains a macro call
// Focus: macro expansion is performed by the preprocessor *before* libclang's
// AST is built, so DOUBLE(x) expands to ((x) * 2) at AST level. The extractor
// sees the expanded form, not the macro token.

#define DOUBLE(x) ((x) * 2)

int use_macro(int n) {
    int r = DOUBLE(n);
    return r;
}
