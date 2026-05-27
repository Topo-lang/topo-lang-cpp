#include <future>
namespace app {
int process(int x) {
    auto f = std::async([](int v) { return v * 2; }, x);
    return f.get();
}
}
