// Fixture 15: C++ non-type template parameter with a default literal.
// Focus: the extractor must capture the default literal expression
// (integer literal `10`) into `defaultValue` (string), independently of
// `defaultType` which only applies to type parameters.

template <int N = 10>
struct Buf {};
