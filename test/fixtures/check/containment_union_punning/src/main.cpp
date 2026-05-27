// Union type punning in function body — containment violation (unsafe type reinterpretation).
namespace app {

int process(int x) {
    union { int i; float f; } u;
    u.i = x;
    return static_cast<int>(u.f);
}

} // namespace app
