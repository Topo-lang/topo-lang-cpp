#ifndef TOPO_SLOT_H
#define TOPO_SLOT_H

// @stability stable
// Public user-facing API. Flat storage (T value_, bool has_value_)
// is pinned; T must be trivially copyable. Lowers to a struct, not
// a discriminated union — no dynamic dispatch.

#include <cstddef>
#include <type_traits>
#include <cassert>

namespace topo {

// Optional-like container for trivially copyable types.
// Always trivially copyable itself (flat struct, no dynamic allocation).
// Header-only, constexpr, C++17.
template <typename T>
struct slot {
    static_assert(std::is_trivially_copyable_v<T>, "topo::slot<T> requires T to be trivially copyable");

    T value_{};
    bool has_value_ = false;

    // Constructors
    constexpr slot() noexcept = default;

    constexpr explicit slot(const T& v) noexcept : value_(v), has_value_(true) {}

    // Observers
    constexpr bool has_value() const noexcept { return has_value_; }
    constexpr explicit operator bool() const noexcept { return has_value_; }

    constexpr T& value() & {
        assert(has_value_ && "topo::slot::value: no value");
        return value_;
    }
    constexpr const T& value() const& {
        assert(has_value_ && "topo::slot::value: no value");
        return value_;
    }

    constexpr T value_or(const T& default_value) const noexcept { return has_value_ ? value_ : default_value; }

    // Modifiers
    constexpr void reset() noexcept {
        value_ = T{};
        has_value_ = false;
    }

    constexpr void emplace(const T& v) noexcept {
        value_ = v;
        has_value_ = true;
    }
};

} // namespace topo

#endif // TOPO_SLOT_H
