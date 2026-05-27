// Fixture 07: `using` type alias in function signature and body
// Focus: the extractor obtains the canonical spelling via clang_getTypeSpelling
// which will resolve the `using Int = int` alias to its underlying type OR
// retain the alias name depending on libclang behaviour. Either way, the
// golden fixes the observed name so future regressions are caught.

using Int = int;

Int compute(Int a) {
    Int b = a + 1;
    return b;
}
