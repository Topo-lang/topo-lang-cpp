// Fixture 16: C++ variadic template parameter (parameter pack).
// Focus: the extractor must recognise `typename... Ts` as a type
// parameter carrying `isVariadic = true` (kind stays "type" — the pack
// flag is orthogonal to the kind discriminator).

template <typename... Ts>
struct Tuple {};
