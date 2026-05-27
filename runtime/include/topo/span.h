#ifndef TOPO_SPAN_H
#define TOPO_SPAN_H

// @stability stable
// Public user-facing API. Layout is (T* ptr_, size_t size_) in that
// order; trivially copyable. IndirectionPass relies on this exact
// shape to lower span<T> to a fat pointer.

#include <array>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace topo {

// Forward declaration for span constructor
template <typename T, std::size_t N>
struct array;

// Non-owning view over a contiguous range of T.
// Trivially copyable (just a pointer + size).
// Header-only, constexpr, C++17.
template <typename T>
struct span {
    T* ptr_ = nullptr;
    std::size_t size_ = 0;

    // Constructors
    constexpr span() noexcept = default;

    constexpr span(T* ptr, std::size_t count) noexcept : ptr_(ptr), size_(count) {}

    // From topo::array<T, N>
    template <std::size_t N>
    constexpr span(array<T, N>& arr) noexcept : ptr_(arr.data()), size_(N) {}

    template <std::size_t N>
    constexpr span(const array<std::remove_const_t<T>, N>& arr) noexcept : ptr_(arr.data()), size_(N) {}

    // From std::array<T, N>
    template <std::size_t N>
    constexpr span(std::array<T, N>& arr) noexcept : ptr_(arr.data()), size_(N) {}

    template <std::size_t N>
    constexpr span(const std::array<std::remove_const_t<T>, N>& arr) noexcept : ptr_(arr.data()), size_(N) {}

    // From std::vector<T>
    span(std::vector<std::remove_const_t<T>>& vec) noexcept : ptr_(vec.data()), size_(vec.size()) {}

    span(const std::vector<std::remove_const_t<T>>& vec) noexcept : ptr_(vec.data()), size_(vec.size()) {}

    // From C-style array T(&)[N]
    template <std::size_t N>
    constexpr span(T (&arr)[N]) noexcept : ptr_(arr), size_(N) {}

    // Element access
    constexpr T& operator[](std::size_t i) const noexcept { return ptr_[i]; }

    constexpr T* data() const noexcept { return ptr_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }

    // Iterators
    constexpr T* begin() const noexcept { return ptr_; }
    constexpr T* end() const noexcept { return ptr_ + size_; }

    // Sub-views
    constexpr span subspan(std::size_t offset, std::size_t count) const noexcept { return {ptr_ + offset, count}; }

    constexpr span first(std::size_t count) const noexcept { return {ptr_, count}; }

    constexpr span last(std::size_t count) const noexcept { return {ptr_ + (size_ - count), count}; }
};

} // namespace topo

#endif // TOPO_SPAN_H
