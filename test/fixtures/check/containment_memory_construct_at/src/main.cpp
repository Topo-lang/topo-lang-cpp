#include <memory>

namespace app {

struct Node { int value; };

void build_node(void* storage, int v) {
    std::construct_at(static_cast<Node*>(storage), Node{v});
}

} // namespace app
