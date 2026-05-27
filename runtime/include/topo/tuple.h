#ifndef TOPO_TUPLE_H
#define TOPO_TUPLE_H

// @stability stable
// Public user-facing API. Field layout (flat _0, _1, ... members),
// arity-2..8 specializations, and the trivially-copyable invariant
// are pinned. Adding a higher arity is backward compatible.

#include <cstddef>
#include <type_traits>
#include <utility>

namespace topo {

// Primary template: undefined (only arity 2-8 specializations provided)
template <typename... Ts>
struct tuple;

// ============================================================================
// X-macro for arity specializations
// ============================================================================
//
// Each TOPO_TUPLE_FIELD(I, T) expands to: T _##I;
// Each arity specialization provides:
//   - Direct flat members (_0, _1, ...)
//   - Default + value constructor
//   - get<I>() member (const&, &, &&)
//
// When all Ti are trivially copyable, topo::tuple<Ti...> is also trivially
// copyable (flat struct, no vtable, no user-defined copy/move).

// -- Arity 2 ----------------------------------------------------------------

template <typename T0, typename T1>
struct tuple<T0, T1> {
    T0 _0;
    T1 _1;

    constexpr tuple() = default;
    constexpr tuple(T0 v0, T1 v1) : _0(std::move(v0)), _1(std::move(v1)) {}

    template <std::size_t I>
    constexpr decltype(auto) get() const& {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() & {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() && {
        if constexpr (I == 0)
            return std::move(_0);
        else if constexpr (I == 1)
            return std::move(_1);
    }
};

// -- Arity 3 ----------------------------------------------------------------

template <typename T0, typename T1, typename T2>
struct tuple<T0, T1, T2> {
    T0 _0;
    T1 _1;
    T2 _2;

    constexpr tuple() = default;
    constexpr tuple(T0 v0, T1 v1, T2 v2) : _0(std::move(v0)), _1(std::move(v1)), _2(std::move(v2)) {}

    template <std::size_t I>
    constexpr decltype(auto) get() const& {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() & {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() && {
        if constexpr (I == 0)
            return std::move(_0);
        else if constexpr (I == 1)
            return std::move(_1);
        else if constexpr (I == 2)
            return std::move(_2);
    }
};

// -- Arity 4 ----------------------------------------------------------------

template <typename T0, typename T1, typename T2, typename T3>
struct tuple<T0, T1, T2, T3> {
    T0 _0;
    T1 _1;
    T2 _2;
    T3 _3;

    constexpr tuple() = default;
    constexpr tuple(T0 v0, T1 v1, T2 v2, T3 v3)
        : _0(std::move(v0)), _1(std::move(v1)), _2(std::move(v2)), _3(std::move(v3)) {}

    template <std::size_t I>
    constexpr decltype(auto) get() const& {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() & {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() && {
        if constexpr (I == 0)
            return std::move(_0);
        else if constexpr (I == 1)
            return std::move(_1);
        else if constexpr (I == 2)
            return std::move(_2);
        else if constexpr (I == 3)
            return std::move(_3);
    }
};

// -- Arity 5 ----------------------------------------------------------------

template <typename T0, typename T1, typename T2, typename T3, typename T4>
struct tuple<T0, T1, T2, T3, T4> {
    T0 _0;
    T1 _1;
    T2 _2;
    T3 _3;
    T4 _4;

    constexpr tuple() = default;
    constexpr tuple(T0 v0, T1 v1, T2 v2, T3 v3, T4 v4)
        : _0(std::move(v0)), _1(std::move(v1)), _2(std::move(v2)), _3(std::move(v3)), _4(std::move(v4)) {}

    template <std::size_t I>
    constexpr decltype(auto) get() const& {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
        else if constexpr (I == 4)
            return (_4);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() & {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
        else if constexpr (I == 4)
            return (_4);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() && {
        if constexpr (I == 0)
            return std::move(_0);
        else if constexpr (I == 1)
            return std::move(_1);
        else if constexpr (I == 2)
            return std::move(_2);
        else if constexpr (I == 3)
            return std::move(_3);
        else if constexpr (I == 4)
            return std::move(_4);
    }
};

// -- Arity 6 ----------------------------------------------------------------

template <typename T0, typename T1, typename T2, typename T3, typename T4, typename T5>
struct tuple<T0, T1, T2, T3, T4, T5> {
    T0 _0;
    T1 _1;
    T2 _2;
    T3 _3;
    T4 _4;
    T5 _5;

    constexpr tuple() = default;
    constexpr tuple(T0 v0, T1 v1, T2 v2, T3 v3, T4 v4, T5 v5)
        : _0(std::move(v0)),
          _1(std::move(v1)),
          _2(std::move(v2)),
          _3(std::move(v3)),
          _4(std::move(v4)),
          _5(std::move(v5)) {}

    template <std::size_t I>
    constexpr decltype(auto) get() const& {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
        else if constexpr (I == 4)
            return (_4);
        else if constexpr (I == 5)
            return (_5);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() & {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
        else if constexpr (I == 4)
            return (_4);
        else if constexpr (I == 5)
            return (_5);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() && {
        if constexpr (I == 0)
            return std::move(_0);
        else if constexpr (I == 1)
            return std::move(_1);
        else if constexpr (I == 2)
            return std::move(_2);
        else if constexpr (I == 3)
            return std::move(_3);
        else if constexpr (I == 4)
            return std::move(_4);
        else if constexpr (I == 5)
            return std::move(_5);
    }
};

// -- Arity 7 ----------------------------------------------------------------

template <typename T0, typename T1, typename T2, typename T3, typename T4, typename T5, typename T6>
struct tuple<T0, T1, T2, T3, T4, T5, T6> {
    T0 _0;
    T1 _1;
    T2 _2;
    T3 _3;
    T4 _4;
    T5 _5;
    T6 _6;

    constexpr tuple() = default;
    constexpr tuple(T0 v0, T1 v1, T2 v2, T3 v3, T4 v4, T5 v5, T6 v6)
        : _0(std::move(v0)),
          _1(std::move(v1)),
          _2(std::move(v2)),
          _3(std::move(v3)),
          _4(std::move(v4)),
          _5(std::move(v5)),
          _6(std::move(v6)) {}

    template <std::size_t I>
    constexpr decltype(auto) get() const& {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
        else if constexpr (I == 4)
            return (_4);
        else if constexpr (I == 5)
            return (_5);
        else if constexpr (I == 6)
            return (_6);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() & {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
        else if constexpr (I == 4)
            return (_4);
        else if constexpr (I == 5)
            return (_5);
        else if constexpr (I == 6)
            return (_6);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() && {
        if constexpr (I == 0)
            return std::move(_0);
        else if constexpr (I == 1)
            return std::move(_1);
        else if constexpr (I == 2)
            return std::move(_2);
        else if constexpr (I == 3)
            return std::move(_3);
        else if constexpr (I == 4)
            return std::move(_4);
        else if constexpr (I == 5)
            return std::move(_5);
        else if constexpr (I == 6)
            return std::move(_6);
    }
};

// -- Arity 8 ----------------------------------------------------------------

template <typename T0, typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7>
struct tuple<T0, T1, T2, T3, T4, T5, T6, T7> {
    T0 _0;
    T1 _1;
    T2 _2;
    T3 _3;
    T4 _4;
    T5 _5;
    T6 _6;
    T7 _7;

    constexpr tuple() = default;
    constexpr tuple(T0 v0, T1 v1, T2 v2, T3 v3, T4 v4, T5 v5, T6 v6, T7 v7)
        : _0(std::move(v0)),
          _1(std::move(v1)),
          _2(std::move(v2)),
          _3(std::move(v3)),
          _4(std::move(v4)),
          _5(std::move(v5)),
          _6(std::move(v6)),
          _7(std::move(v7)) {}

    template <std::size_t I>
    constexpr decltype(auto) get() const& {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
        else if constexpr (I == 4)
            return (_4);
        else if constexpr (I == 5)
            return (_5);
        else if constexpr (I == 6)
            return (_6);
        else if constexpr (I == 7)
            return (_7);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() & {
        if constexpr (I == 0)
            return (_0);
        else if constexpr (I == 1)
            return (_1);
        else if constexpr (I == 2)
            return (_2);
        else if constexpr (I == 3)
            return (_3);
        else if constexpr (I == 4)
            return (_4);
        else if constexpr (I == 5)
            return (_5);
        else if constexpr (I == 6)
            return (_6);
        else if constexpr (I == 7)
            return (_7);
    }
    template <std::size_t I>
    constexpr decltype(auto) get() && {
        if constexpr (I == 0)
            return std::move(_0);
        else if constexpr (I == 1)
            return std::move(_1);
        else if constexpr (I == 2)
            return std::move(_2);
        else if constexpr (I == 3)
            return std::move(_3);
        else if constexpr (I == 4)
            return std::move(_4);
        else if constexpr (I == 5)
            return std::move(_5);
        else if constexpr (I == 6)
            return std::move(_6);
        else if constexpr (I == 7)
            return std::move(_7);
    }
};

// ============================================================================
// ADL free function: topo::get<I>(t)
// ============================================================================

template <std::size_t I, typename... Ts>
constexpr decltype(auto) get(const tuple<Ts...>& t) {
    return t.template get<I>();
}

template <std::size_t I, typename... Ts>
constexpr decltype(auto) get(tuple<Ts...>& t) {
    return t.template get<I>();
}

template <std::size_t I, typename... Ts>
constexpr decltype(auto) get(tuple<Ts...>&& t) {
    return std::move(t).template get<I>();
}

} // namespace topo

// ============================================================================
// std::tuple_size / std::tuple_element specializations (structured bindings)
// ============================================================================

namespace std {

// -- tuple_size for each arity -----------------------------------------------

template <typename T0, typename T1>
struct tuple_size<topo::tuple<T0, T1>> : integral_constant<size_t, 2> {};

template <typename T0, typename T1, typename T2>
struct tuple_size<topo::tuple<T0, T1, T2>> : integral_constant<size_t, 3> {};

template <typename T0, typename T1, typename T2, typename T3>
struct tuple_size<topo::tuple<T0, T1, T2, T3>> : integral_constant<size_t, 4> {};

template <typename T0, typename T1, typename T2, typename T3, typename T4>
struct tuple_size<topo::tuple<T0, T1, T2, T3, T4>> : integral_constant<size_t, 5> {};

template <typename T0, typename T1, typename T2, typename T3, typename T4, typename T5>
struct tuple_size<topo::tuple<T0, T1, T2, T3, T4, T5>> : integral_constant<size_t, 6> {};

template <typename T0, typename T1, typename T2, typename T3, typename T4, typename T5, typename T6>
struct tuple_size<topo::tuple<T0, T1, T2, T3, T4, T5, T6>> : integral_constant<size_t, 7> {};

template <typename T0, typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7>
struct tuple_size<topo::tuple<T0, T1, T2, T3, T4, T5, T6, T7>> : integral_constant<size_t, 8> {};

// -- tuple_element: helper to index into a topo::tuple type list -------------

namespace detail {

template <size_t I, typename... Ts>
struct topo_tuple_element;

template <typename T0, typename... Rest>
struct topo_tuple_element<0, T0, Rest...> {
    using type = T0;
};

template <size_t I, typename T0, typename... Rest>
struct topo_tuple_element<I, T0, Rest...> : topo_tuple_element<I - 1, Rest...> {};

} // namespace detail

template <size_t I, typename T0, typename T1>
struct tuple_element<I, topo::tuple<T0, T1>> {
    using type = typename detail::topo_tuple_element<I, T0, T1>::type;
};

template <size_t I, typename T0, typename T1, typename T2>
struct tuple_element<I, topo::tuple<T0, T1, T2>> {
    using type = typename detail::topo_tuple_element<I, T0, T1, T2>::type;
};

template <size_t I, typename T0, typename T1, typename T2, typename T3>
struct tuple_element<I, topo::tuple<T0, T1, T2, T3>> {
    using type = typename detail::topo_tuple_element<I, T0, T1, T2, T3>::type;
};

template <size_t I, typename T0, typename T1, typename T2, typename T3, typename T4>
struct tuple_element<I, topo::tuple<T0, T1, T2, T3, T4>> {
    using type = typename detail::topo_tuple_element<I, T0, T1, T2, T3, T4>::type;
};

template <size_t I, typename T0, typename T1, typename T2, typename T3, typename T4, typename T5>
struct tuple_element<I, topo::tuple<T0, T1, T2, T3, T4, T5>> {
    using type = typename detail::topo_tuple_element<I, T0, T1, T2, T3, T4, T5>::type;
};

template <size_t I, typename T0, typename T1, typename T2, typename T3, typename T4, typename T5, typename T6>
struct tuple_element<I, topo::tuple<T0, T1, T2, T3, T4, T5, T6>> {
    using type = typename detail::topo_tuple_element<I, T0, T1, T2, T3, T4, T5, T6>::type;
};

template <size_t I,
          typename T0,
          typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6,
          typename T7>
struct tuple_element<I, topo::tuple<T0, T1, T2, T3, T4, T5, T6, T7>> {
    using type = typename detail::topo_tuple_element<I, T0, T1, T2, T3, T4, T5, T6, T7>::type;
};

} // namespace std

#endif // TOPO_TUPLE_H
