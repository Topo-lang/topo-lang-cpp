#ifndef TOPO_VIEW_H
#define TOPO_VIEW_H

// @stability stable
// Public user-facing API. Single-pointer layout, zero overhead,
// trivially copyable. Carries Topo `readonly` metadata in IR.

#include <cstddef>
#include <type_traits>

namespace topo {

// Read-only non-owning reference wrapper.
// Trivially copyable (single pointer), zero overhead.
// Semantically equivalent to const T&, but carries Topo readonly metadata in IR.
// Header-only, constexpr, C++17.
template <typename T>
struct view {
    const T* ptr_;

    constexpr view(const T& ref) noexcept : ptr_(&ref) {}

    constexpr const T& get() const noexcept { return *ptr_; }
    constexpr const T* operator->() const noexcept { return ptr_; }
    constexpr const T& operator*() const noexcept { return *ptr_; }

    // Implicit conversion to const T&
    constexpr operator const T&() const noexcept { return *ptr_; }
};

} // namespace topo

#endif // TOPO_VIEW_H
