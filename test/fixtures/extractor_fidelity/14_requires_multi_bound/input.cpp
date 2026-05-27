// Fixture 14: C++20 `requires`-clause multi-bound on a class template.
// Focus: libclang exposes no `requires`-clause API, so the extractor must
// scan tokens between the `template <...>` header and the body, recognise
// `requires A<T> && B<T>`, and graduate the type-param from single-bound
// to multi-bound (`bounds: [TypeNode]`).
//
// Concept declarations are in-TU because topo-extract-cpp parses without
// system include paths.

template <typename T>
concept Sortable = true;

template <typename T>
concept Hashable = true;

template <typename T> requires Sortable<T> && Hashable<T>
struct Box {};
