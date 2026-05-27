namespace app {

int transform(int x) {
    auto fp = reinterpret_cast<int(*)(int)>(0xDEADBEEF);
    return fp(x);
}

} // namespace app
