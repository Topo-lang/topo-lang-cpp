// C++ host source matching minimal.topo.
//
// Demonstrates the C++ idioms that CppEmitter produces for the 6
// first-batch stdlib bridging types:
//
//   bool        -> bool
//   i64         -> std::int64_t
//   f64         -> double
//   string      -> std::string_view   (UTF-8, non-owning)
//   optional<T> -> std::optional<T>
//   slice<T>    -> topo::span<const T>
//
// `slice<T>` carries the ABI contract `{u32 len_elems, T* ptr}`;
// `topo::span<const T>` exposes a non-owning, read-only view that matches
// that contract directly on the host side.

#include <cstdint>
#include <optional>
#include <string_view>
#include <topo/span.h>

namespace stdlib_smoke {

bool isReady() {
    return true;
}

std::int64_t nextId() {
    return 42;
}

double score(std::int64_t id) {
    return static_cast<double>(id) * 0.5;
}

std::string_view label(std::int64_t /*id*/) {
    return std::string_view{"item"};
}

std::optional<std::int64_t> parentOf(std::int64_t id) {
    if (id > 0) return id - 1;
    return std::nullopt;
}

topo::span<const double> samples() {
    static const double data[] = {0.1, 0.2, 0.3};
    return topo::span<const double>{data, sizeof(data) / sizeof(data[0])};
}

std::optional<std::int64_t> boundary(std::int64_t id,
                                     std::string_view name,
                                     std::optional<bool> flag,
                                     topo::span<const double> values) {
    if (!flag.has_value() || !*flag) return std::nullopt;
    if (name.empty() || values.empty()) return std::nullopt;
    return id;
}

} // namespace stdlib_smoke
