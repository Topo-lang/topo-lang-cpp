#ifndef TOPO_ARRAY_H
#define TOPO_ARRAY_H

// @stability stable
//
// Public user-facing API. Flat storage (`T data_[N]`), trivial copyability
// when T is trivial, and the constexpr accessor surface are pinned. Lowers
// to a plain C array for codegen.
//
// Why this is NOT a `using topo::array = std::array;` alias: clang elides
// `std::array` as a named struct in IR (its only purpose is the inner
// `[N x T]` member, and opaque pointers let codegen skip the wrapper),
// which erases the AoS-eligibility marker DataLayoutPass detects. Keeping
// `topo::array` as its own concrete struct ensures `%"struct.topo::array"`
// appears in IR so the pass can find candidate wrappers. DataLayoutPass
// ALSO accepts `struct.std::array` shapes (libstdc++ / libc++ inline-ns
// forms) for the rare case where they do materialize.

#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace topo {

// Fixed-size array container.
// Flat struct with alignof(T) alignment.  Trivially copyable when T is.
// Header-only, constexpr, C++17.
template <typename T, std::size_t N>
struct array {
    static_assert(N > 0, "topo::array size must be > 0");

    T data_[N];

    // Element access
    constexpr T& operator[](std::size_t i) noexcept { return data_[i]; }
    constexpr const T& operator[](std::size_t i) const noexcept { return data_[i]; }

    constexpr T& at(std::size_t i) {
        assert(i < N && "topo::array::at: index out of range");
        return data_[i];
    }
    constexpr const T& at(std::size_t i) const {
        assert(i < N && "topo::array::at: index out of range");
        return data_[i];
    }

    constexpr T& front() noexcept { return data_[0]; }
    constexpr const T& front() const noexcept { return data_[0]; }

    constexpr T& back() noexcept { return data_[N - 1]; }
    constexpr const T& back() const noexcept { return data_[N - 1]; }

    // Iterators (pointer-based)
    constexpr T* begin() noexcept { return data_; }
    constexpr const T* begin() const noexcept { return data_; }
    constexpr T* end() noexcept { return data_ + N; }
    constexpr const T* end() const noexcept { return data_ + N; }

    // Capacity
    constexpr std::size_t size() const noexcept { return N; }
    constexpr bool empty() const noexcept { return false; }

    // Operations
    constexpr void fill(const T& value) {
        for (std::size_t i = 0; i < N; ++i)
            data_[i] = value;
    }

    constexpr T* data() noexcept { return data_; }
    constexpr const T* data() const noexcept { return data_; }

    // Structured bindings support: member get<I>()
    template <std::size_t I>
    constexpr T& get() & noexcept {
        static_assert(I < N, "topo::array::get: index out of range");
        return data_[I];
    }
    template <std::size_t I>
    constexpr const T& get() const& noexcept {
        static_assert(I < N, "topo::array::get: index out of range");
        return data_[I];
    }
    template <std::size_t I>
    constexpr T&& get() && noexcept {
        static_assert(I < N, "topo::array::get: index out of range");
        return static_cast<T&&>(data_[I]);
    }
};

// ADL free function: topo::get<I>(arr)
template <std::size_t I, typename T, std::size_t N>
constexpr T& get(array<T, N>& a) noexcept {
    return a.template get<I>();
}
template <std::size_t I, typename T, std::size_t N>
constexpr const T& get(const array<T, N>& a) noexcept {
    return a.template get<I>();
}
template <std::size_t I, typename T, std::size_t N>
constexpr T&& get(array<T, N>&& a) noexcept {
    return static_cast<T&&>(a.template get<I>());
}

} // namespace topo

// Structured bindings: std::tuple_size / std::tuple_element
namespace std {

template <typename T, size_t N>
struct tuple_size<topo::array<T, N>> : integral_constant<size_t, N> {};

template <size_t I, typename T, size_t N>
struct tuple_element<I, topo::array<T, N>> {
    static_assert(I < N, "index out of range");
    using type = T;
};

} // namespace std

#endif // TOPO_ARRAY_H
