#ifndef TOPO_APP_MODEL_H
#define TOPO_APP_MODEL_H

// @stability provisional
// Public data model for topo-app's in-memory graph. Field shapes are
// expected to expand as the Functor model lands more cases; today's
// shapes are intended to be the eventual stable surface, but the
// `provisional` marker is honest until the test suite covers the full
// topo-app mapping grid.

// In-memory logic graph: the single source of truth a topo-app C++
// program builds by registration. This is the C++ projection of the
// Python reference's _graph.py — same data model, same semantic-equality
// contract, no behaviour beyond structural meaning.
//
// The graph is intentionally a plain data model. Emission, read-back and
// checking are separate concerns (app_emit.h / app_readback.h /
// app_check.h) that consume this model so the round-trip can be reasoned
// about as data, not side effects.
//
// Vocabulary reuse: the stdlib scalar set and the ordered
// std::vector<std::pair<std::string, ...>> record shape are taken
// straight from the config port (config_model.h's ValueKind scalar bands
// and Record = vector<pair<string, ConfigValue>>). The difference is
// type-level vs value-level: config_model.h's Record maps field name to a
// *value*; a handler schema maps field name to a *type spelling*. We
// keep the same shape and the same scalar bands so the two models read
// identically and a reader who knows one knows the other.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace topo::app {

// Stdlib scalar bands. Deliberately the same four bands the Python host
// binds (int / float / bool / str) and the same conceptual set as
// config_model.h's ValueKind {Bool, Int, Float, String} scalars — a
// handler In/Out reuses exactly that vocabulary, no wider.
enum class Scalar { Int, Float, Bool, Str };

class TypeRef;

// A field of a stdlib record<...>: ordered (name, type) pair. Same shape
// as config_model.h's Record entry (vector<pair<string, ...>>), the
// difference being the second member is a type spelling, not a value.
using RecordField = std::pair<std::string, TypeRef>;

// A topo type as it will be spelled in .topo. Exactly one of scalar /
// record is populated; void (no input / terminal) is represented by the
// *absence* of a TypeRef at the use site (std::optional empty), never by
// a TypeRef instance — mirrors the Python TypeRef contract verbatim.
class TypeRef {
public:
    static TypeRef of_scalar(Scalar s) {
        TypeRef t;
        t.scalar_ = s;
        return t;
    }
    static TypeRef of_record(std::vector<RecordField> fields) {
        TypeRef t;
        t.record_ = std::move(fields);
        return t;
    }

    bool is_record() const { return record_.has_value(); }

    // The .topo spelling. Scalars use the alias names the C++ host binds
    // (int/float/bool/str — see app_emit.h's preamble), matching the
    // Python projection's int/float/bool/str spelling so emitted .topo
    // and its AST dump are byte-identical across hosts.
    std::string topo() const {
        if (record_.has_value()) {
            std::string inner;
            for (std::size_t i = 0; i < record_->size(); ++i) {
                if (i) inner += ", ";
                inner += (*record_)[i].first + ": " + (*record_)[i].second.topo();
            }
            return "record<" + inner + ">";
        }
        switch (*scalar_) {
            case Scalar::Int: return "int";
            case Scalar::Float: return "float";
            case Scalar::Bool: return "bool";
            case Scalar::Str: return "str";
        }
        return {};  // unreachable; all bands handled
    }

    bool operator==(const TypeRef& o) const {
        return scalar_ == o.scalar_ && record_ == o.record_;
    }

private:
    std::optional<Scalar> scalar_;
    std::optional<std::vector<RecordField>> record_;
};

// A registered logic unit. in_type is empty for a source handler.
struct Handler {
    std::string name;
    std::optional<TypeRef> in_type;
    TypeRef out_type;

    // The single input parameter is conventionally named `in` to match
    // the handler/flow HandlerInput form; a source handler has no
    // parameter. Identical to the Python reference's signature().
    std::string signature() const {
        std::string param =
            in_type.has_value() ? in_type->topo() + " in" : std::string();
        return "handler " + name + "(" + param + ") -> " + out_type.topo() +
               ";";
    }
};

// A pipeline edge inside a flow. target empty == terminal (source ->
// void;).
struct Edge {
    std::string source;
    std::optional<std::string> target;

    bool is_terminal() const { return !target.has_value(); }

    bool operator==(const Edge& o) const {
        return source == o.source && target == o.target;
    }
};

struct Flow {
    std::string name;
    std::vector<Edge> edges;
};

// The whole program: one namespace, the handlers, one flow. A single
// namespace + single flow keeps the bridge minimal while still
// exercising every mapping rule the topo-app design commits to.
class Graph {
public:
    explicit Graph(std::string namespace_name)
        : namespace_(std::move(namespace_name)) {}

    const std::string& namespace_name() const { return namespace_; }

    std::vector<Handler>& handlers() { return handlers_; }
    const std::vector<Handler>& handlers() const { return handlers_; }

    bool has_flow() const { return flow_.has_value(); }
    Flow& flow() { return *flow_; }
    const Flow& flow() const { return *flow_; }
    void set_flow(Flow f) { flow_ = std::move(f); }

    const Handler* handler(const std::string& name) const {
        for (const auto& h : handlers_)
            if (h.name == name) return &h;
        return nullptr;
    }

    // --- Semantic equality (the round-trip's headline acceptance) ------
    //
    // A canonical, order-insensitive description of the graph's meaning.
    // Two graphs are semantically equivalent iff their keys are equal.
    // Handler order and edge order do not change meaning (stage topology
    // is derived from the edge set), so both are sorted — identical to
    // the Python reference's semantic_key().
    struct SemanticKey {
        std::string namespace_name;
        std::optional<std::string> flow_name;
        // (name, optional in-spelling, out-spelling) sorted
        std::vector<std::tuple<std::string, std::optional<std::string>,
                               std::string>>
            handlers;
        // (source, optional target) sorted
        std::vector<std::pair<std::string, std::optional<std::string>>> edges;

        bool operator==(const SemanticKey& o) const {
            return namespace_name == o.namespace_name &&
                   flow_name == o.flow_name && handlers == o.handlers &&
                   edges == o.edges;
        }
    };

    SemanticKey semantic_key() const {
        SemanticKey k;
        k.namespace_name = namespace_;
        for (const auto& h : handlers_) {
            std::optional<std::string> in;
            if (h.in_type.has_value()) in = h.in_type->topo();
            k.handlers.emplace_back(h.name, in, h.out_type.topo());
        }
        std::sort(k.handlers.begin(), k.handlers.end());
        if (flow_.has_value()) {
            k.flow_name = flow_->name;
            for (const auto& e : flow_->edges)
                k.edges.emplace_back(e.source, e.target);
            std::sort(k.edges.begin(), k.edges.end());
        }
        return k;
    }

    bool equivalent_to(const Graph& other) const {
        return semantic_key() == other.semantic_key();
    }

private:
    std::string namespace_;
    std::vector<Handler> handlers_;
    std::optional<Flow> flow_;
};

}  // namespace topo::app

#endif  // TOPO_APP_MODEL_H
