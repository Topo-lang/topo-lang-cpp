// Fixture 13: free function template
// Focus: libclang exposes function templates as CXCursor_FunctionTemplate
// rather than CXCursor_FunctionDecl. The extractor must handle this kind
// so containment / visibility checks can see calls inside template bodies.
//
// The bare type parameter T is now recovered into the function's
// templateParams (declaration-level generics); the dependent-type
// signature and the body are still recorded as-is.

template <typename T>
T identity(T value) {
    return value;
}
