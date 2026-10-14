#include <cstdlib>
namespace app {
class Log {
public:
    void write() {
        hidden();
    }
private:
    void hidden() {
        auto value = std::getenv("TOPO_AUDIT_FIXTURE");
    }
};
class Other {
public:
    void hidden() {
        auto value = std::getenv("TOPO_AUDIT_FIXTURE");
    }
};
}
