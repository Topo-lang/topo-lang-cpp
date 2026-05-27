#ifndef TOPO_TABLE_H
#define TOPO_TABLE_H

// @stability stable
// Public user-facing API. `basic_table<Align, Columns...>` arity-2..8
// + the aligned-storage layout are pinned. Internal helpers (aligned
// allocation RAII, column accessors) live in `topo::detail::` and
// are exempt from this stability tier.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include <topo/span.h>
#include <topo/tuple.h>

namespace topo {

namespace detail {

// RAII wrapper for over-aligned heap allocation.
// Uses C++17 aligned new/delete with compile-time alignment value.
template <typename T, std::size_t Align>
struct aligned_ptr {
    T* ptr_ = nullptr;
    std::size_t count_ = 0;

    aligned_ptr() = default;

    explicit aligned_ptr(std::size_t n)
        : ptr_(static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t(Align)))), count_(n) {
        // Value-initialize all elements
        for (std::size_t i = 0; i < n; ++i)
            new (ptr_ + i) T{};
    }

    ~aligned_ptr() {
        if (ptr_) {
            for (std::size_t i = 0; i < count_; ++i)
                ptr_[i].~T();
            ::operator delete(ptr_, std::align_val_t(Align));
        }
    }

    // Move only
    aligned_ptr(const aligned_ptr&) = delete;
    aligned_ptr& operator=(const aligned_ptr&) = delete;

    aligned_ptr(aligned_ptr&& other) noexcept : ptr_(other.ptr_), count_(other.count_) {
        other.ptr_ = nullptr;
        other.count_ = 0;
    }

    aligned_ptr& operator=(aligned_ptr&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                for (std::size_t i = 0; i < count_; ++i)
                    ptr_[i].~T();
                ::operator delete(ptr_, std::align_val_t(Align));
            }
            ptr_ = other.ptr_;
            count_ = other.count_;
            other.ptr_ = nullptr;
            other.count_ = 0;
        }
        return *this;
    }

    T* get() noexcept { return ptr_; }
    const T* get() const noexcept { return ptr_; }
};

} // namespace detail

// Columnar (SoA) storage container with over-aligned column allocation.
// Each column is independently heap-allocated with Align-byte alignment.
// Column count constrained to 2-8 (matching topo::tuple arity range).
//
// Template parameters:
//   Align    — allocation alignment in bytes (default 64 = cache line)
//   Columns  — column element types
template <std::size_t Align, typename... Columns>
class basic_table {
    static_assert(sizeof...(Columns) >= 2 && sizeof...(Columns) <= 8,
                  "topo::table requires 2-8 columns (matching topo::tuple arity)");
    static_assert(Align > 0 && (Align & (Align - 1)) == 0, "Alignment must be a power of two");

public:
    static constexpr std::size_t column_count = sizeof...(Columns);

    explicit basic_table(std::size_t rows) : rows_(rows), columns_(make_columns(rows)) {}

    // Move only (heap-owning container)
    basic_table(const basic_table&) = delete;
    basic_table& operator=(const basic_table&) = delete;
    basic_table(basic_table&&) = default;
    basic_table& operator=(basic_table&&) = default;

    std::size_t size() const noexcept { return rows_; }
    static constexpr std::size_t alignment() noexcept { return Align; }

    // Column access — returns raw pointer to aligned contiguous array
    template <std::size_t I>
    auto* column() noexcept {
        static_assert(I < column_count, "Column index out of range");
        return std::get<I>(columns_).get();
    }

    template <std::size_t I>
    const auto* column() const noexcept {
        static_assert(I < column_count, "Column index out of range");
        return std::get<I>(columns_).get();
    }

    // Column as topo::span
    template <std::size_t I>
    auto column_span() noexcept {
        using T = std::tuple_element_t<I, std::tuple<Columns...>>;
        return topo::span<T>(column<I>(), rows_);
    }

    template <std::size_t I>
    auto column_span() const noexcept {
        using T = std::tuple_element_t<I, std::tuple<Columns...>>;
        return topo::span<const T>(column<I>(), rows_);
    }

    // Row access — returns topo::tuple of copies
    topo::tuple<Columns...> row(std::size_t i) const { return row_impl(i, std::index_sequence_for<Columns...>{}); }

    // Set row from topo::tuple
    void set_row(std::size_t i, const topo::tuple<Columns...>& t) {
        set_row_impl(i, t, std::index_sequence_for<Columns...>{});
    }

private:
    std::size_t rows_;
    std::tuple<detail::aligned_ptr<Columns, Align>...> columns_;

    static std::tuple<detail::aligned_ptr<Columns, Align>...> make_columns(std::size_t rows) {
        return std::tuple<detail::aligned_ptr<Columns, Align>...>{detail::aligned_ptr<Columns, Align>(rows)...};
    }

    template <std::size_t... Is>
    topo::tuple<Columns...> row_impl(std::size_t i, std::index_sequence<Is...>) const {
        return topo::tuple<Columns...>{std::get<Is>(columns_).get()[i]...};
    }

    template <std::size_t... Is>
    void set_row_impl(std::size_t i, const topo::tuple<Columns...>& t, std::index_sequence<Is...>) {
        ((std::get<Is>(columns_).get()[i] = t.template get<Is>()), ...);
    }
};

// Default alias: 64-byte aligned (cache line / AVX-512)
template <typename... Columns>
using table = basic_table<64, Columns...>;

} // namespace topo

#endif // TOPO_TABLE_H
