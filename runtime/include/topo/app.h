#ifndef TOPO_APP_H
#define TOPO_APP_H

// @stability stable
// Public user-facing C++ projection of topo-app: App, handler(),
// flow(), parallel(). Internal carriers (Parallel) live under
// `topo::app::detail::`. Lower layers in app_model/app_emit/etc.
// have their own stability tier.

// topo-app C++ surface: idiomatic registration, not a macro DSL.
//
// The proposal fixes the philosophy (pure-Functor handler model) and
// leaves each topo-lang to project it onto its own idioms. The C++
// projection is template function_traits over an ordinary function
// pointer plus a plain registration call: registering a handler is
// ordinary C++, In/Out are *read from the function type* (never
// re-declared), and a flow is declared by listing the chain. No new
// syntax, no codegen, no source scanner — this is a runtime registration
// bridge, deliberately NOT a compile-time static-analysis front-end.
//
// A handler stays a normal callable after registration, so it remains
// independently invocable and unit-testable with zero framework
// bootstrap — a free consequence of the Functor model, mirroring the
// Python reference (app.py) one-to-one.
//
// Record field names without reflection: C++ has no built-in reflection,
// so a record's field *names* cannot be recovered from a plain struct.
// This is resolved exactly the way the Python host resolves it — the
// user spells the record as topo::app::Record<Field<"id", int>, ...>,
// carrying the field names as explicit, ordered compile-time metadata on
// the type itself. function_traits then only has to extract the
// parameter/return *types*; the record type already knows its own field
// names. This is the same contract as Python's
// Record[("id", int), ("amount", float)] (names are explicit metadata,
// never inferred) — parity, not degradation.

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <topo/app_model.h>

namespace topo::app {

// --- Compile-time field name (string literal as a type parameter) ---------
//
// A structural NTTP wrapper so a field name can be written inline as
// Field<"id", int>. C++20 allows class-type NTTPs; this fixed_string is
// the standard idiom for "string literal as template argument".
template <std::size_t N>
struct fixed_string {
    char data[N]{};
    constexpr fixed_string(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    constexpr std::string str() const { return std::string(data); }
};

// One ordered, named record field: Field<"amount", double>. The name is
// compile-time metadata on the type — the C++ analogue of the Python
// host's Annotated[T, "name"].
template <fixed_string Name, typename T>
struct Field {
    static constexpr auto name = Name;
    using type = T;
};

// A stdlib record<...> spelled ergonomically as
// Record<Field<"id", int>, Field<"amount", double>>. Order is the
// template-argument order and is stable — the C++ analogue of the Python
// host's order-preserving tuple[Annotated[...], ...].
template <typename... Fields>
struct Record {
    static_assert(sizeof...(Fields) > 0,
                  "a stdlib record<...> must declare at least one field "
                  "(core Sema rejects record<> upstream)");
};

// --- Scalar mapping -------------------------------------------------------
//
// The four stdlib bands the C++ host binds. A C++ type is mapped to a
// band the same way the Python host maps int/float/bool/str. bool is
// matched before the integral fallthrough so it is never silently
// widened to int — the same precedence config_model.h documents for its
// ValueKind.
template <typename T>
constexpr Scalar scalar_of() {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<U, bool>) {
        return Scalar::Bool;
    } else if constexpr (std::is_same_v<U, std::string> ||
                         std::is_same_v<U, const char*>) {
        return Scalar::Str;
    } else if constexpr (std::is_floating_point_v<U>) {
        return Scalar::Float;
    } else if constexpr (std::is_integral_v<U>) {
        return Scalar::Int;
    } else {
        // No `false` literal in a static_assert dependent on T without a
        // dependent expression; this always-false trips only when the
        // branch is actually instantiated for an unsupported type.
        static_assert(sizeof(T) == 0,
                      "unsupported handler type; use an integral / floating "
                      "/ bool / string scalar, or topo::app::Record<...> "
                      "for a multi-field structure");
        return Scalar::Int;
    }
}

// --- Type -> TypeRef ------------------------------------------------------

template <typename T>
struct type_mapper {
    static TypeRef ref() { return TypeRef::of_scalar(scalar_of<T>()); }
};

template <typename... Fields>
struct type_mapper<Record<Fields...>> {
    static TypeRef ref() {
        std::vector<RecordField> fields;
        // Fold over the parameter pack preserves declaration order, which
        // is the record's field order — the explicit, stable ordering the
        // contract requires.
        (fields.emplace_back(Fields::name.str(),
                             type_mapper<typename Fields::type>::ref()),
         ...);
        return TypeRef::of_record(std::move(fields));
    }
};

// --- function_traits ------------------------------------------------------
//
// Extract In/Out from a plain function pointer at compile time. A handler
// is a pure Functor: at most one input. Zero inputs == a source handler
// (In is void, represented as an empty optional). More than one input is
// rejected with the same intent as the core Parser ("a handler takes at
// most one input parameter") so the user never reaches emission with an
// unrepresentable signature — same guard as the Python host's
// reflect_signature().
template <typename F>
struct function_traits;

template <typename R>
struct function_traits<R (*)()> {
    static std::optional<TypeRef> in() { return std::nullopt; }
    static TypeRef out() { return type_mapper<R>::ref(); }
};

template <typename R, typename A>
struct function_traits<R (*)(A)> {
    static std::optional<TypeRef> in() { return type_mapper<A>::ref(); }
    static TypeRef out() { return type_mapper<R>::ref(); }
};

// Any 2+-arg arity is intentionally NOT specialised: a handler with more
// than one input is a hard error caught at compile time (no matching
// specialisation), the C++ analogue of the Python TypeError.

// --- App ------------------------------------------------------------------

namespace detail { class Parallel; }

// A topo-app program: the in-memory logic graph plus the callables. One
// App owns one namespace and (for this bridge) one flow — enough to
// exercise every proposal mapping rule without productionizing.
class App {
public:
    explicit App(std::string namespace_name)
        : graph_(std::move(namespace_name)) {}

    // Register a logic unit. The function stays an ordinary callable and
    // is returned unchanged so it remains independently invocable with
    // zero framework bootstrap. In/Out are reflected from the function
    // type, never re-declared (no redundant declaration of types).
    template <typename F>
    F handler(F fn, const std::string& name,
              const std::string& /*doc*/ = "") {
        using Traits = function_traits<F>;
        graph_.handlers().push_back(
            Handler{name, Traits::in(), Traits::out()});
        return fn;  // unchanged: still a plain, independently callable fn
    }

    // Declare a linear logic chain: flow("p", a, b, c) becomes edges
    // a->b->c->void. parallel(...) members fan in/out from the same
    // neighbours (same-source / same-sink == same-stage parallel
    // candidates, per the proposal's mapping table). Stages accept either
    // a registered handler name (std::string) or parallel(...).
    template <typename... Stages>
    void flow(const std::string& name, Stages... stages) {
        std::vector<std::vector<std::string>> stage_names;
        (stage_names.push_back(names_of(stages)), ...);

        Flow f;
        f.name = name;
        for (std::size_t i = 0; i + 1 < stage_names.size(); ++i)
            for (const auto& src : stage_names[i])
                for (const auto& tgt : stage_names[i + 1])
                    f.edges.push_back(Edge{src, tgt});
        for (const auto& src : stage_names.back())
            f.edges.push_back(Edge{src, std::nullopt});  // terminal -> void
        graph_.set_flow(std::move(f));
    }

    Graph& graph() { return graph_; }
    const Graph& graph() const { return graph_; }

private:
    static std::vector<std::string> names_of(const std::string& s) {
        return {s};
    }
    static std::vector<std::string> names_of(const char* s) {
        return {std::string(s)};
    }
    static std::vector<std::string> names_of(const detail::Parallel& p);

    Graph graph_;
};

// Independent units on the same input == same-stage parallel candidates.
// Purity of these is enforced by core PurityCheck after emission, not
// self-asserted (mirrors the Python host's parallel()). The class is
// in `detail::` to mark it as an internal carrier; users construct it
// only via the `parallel(...)` factory and pass it to `App::flow()`.
namespace detail {
class Parallel {
public:
    explicit Parallel(std::vector<std::string> members)
        : members_(std::move(members)) {}
    const std::vector<std::string>& members() const { return members_; }

private:
    std::vector<std::string> members_;
};
} // namespace detail

inline std::vector<std::string> App::names_of(const detail::Parallel& p) {
    return p.members();
}

inline detail::Parallel parallel_impl(std::vector<std::string> members) {
    return detail::Parallel(std::move(members));
}

template <typename... Names>
detail::Parallel parallel(Names... names) {
    return detail::Parallel(std::vector<std::string>{std::string(names)...});
}

}  // namespace topo::app

#endif  // TOPO_APP_H
