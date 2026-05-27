#ifndef TOPO_ARENA_H
#define TOPO_ARENA_H

// @stability stable
// Public user-facing API: `topo::Arena` RAII wrapper over the
// libtopo-arena C ABI. Underlying ABI is versioned at
// TOPO_ARENA_ABI_VERSION; this header is the idiomatic C++ wrapper
// and tracks ABI v1 surface.

#include "topo/rt/arena_rt.h"

#include <cstddef>
#include <new>
#include <utility>

namespace topo {

/// RAII wrapper around the arena C API.
/// Provides bump allocation with batch reset.
class Arena {
public:
    explicit Arena(std::size_t initialCapacity = 4096) : handle_(topo_arena_create(initialCapacity)) {}

    ~Arena() {
        if (handle_) topo_arena_destroy(handle_);
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            if (handle_) topo_arena_destroy(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    /// Allocate raw memory.
    void* alloc(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) {
        return topo_arena_alloc(handle_, size, alignment);
    }

    /// Allocate and construct an object of type T.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = alloc(sizeof(T), alignof(T));
        if (!mem) return nullptr;
        return new (mem) T(std::forward<Args>(args)...);
    }

    /// Reset the arena (invalidates all allocations).
    void reset() { topo_arena_reset(handle_); }

    /// Query bytes used.
    std::size_t bytesUsed() const { return topo_arena_bytes_used(handle_); }

    /// Query total capacity.
    std::size_t capacity() const { return topo_arena_capacity(handle_); }

    /// Check if the arena is valid.
    explicit operator bool() const { return handle_ != nullptr; }

private:
    topo_arena_t handle_;
};

} // namespace topo

#endif // TOPO_ARENA_H
