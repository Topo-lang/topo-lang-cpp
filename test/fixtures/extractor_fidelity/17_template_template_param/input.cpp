// Fixture 17: C++ template-template parameter.
// Focus: the extractor must recognise `template<typename> class C` as a
// template-template parameter (kind = "template") and record the single
// inner type parameter into `innerParams`. MVP shape only — the inner
// param is a plain unnamed type param.

template <template<typename> class C>
struct Holder {};
