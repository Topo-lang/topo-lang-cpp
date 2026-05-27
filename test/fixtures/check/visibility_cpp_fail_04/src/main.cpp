// `graphics::render` reaches into `engine::backend::flushBuffers`, which
// is declared private in namespace engine::backend. Expected: one violation.

namespace engine {

void orchestrate() {
    // no-op
}

void run() {
    orchestrate();
}

} // namespace engine

namespace engine {
namespace backend {

void flushBuffers() {
    // private
}

} // namespace backend
} // namespace engine

namespace graphics {

void render() {
    engine::backend::flushBuffers();  // cross-namespace private — violation
}

} // namespace graphics
