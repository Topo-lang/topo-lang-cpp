#include "CppEmitter.h"
#include "topo/Stdlib/Types.h"
#include <map>
#include <sstream>

namespace topo::transpile {

// Stdlib bridging types. The 6 first-batch types route
// through an explicit `type.isStdlib()` branch in emitType() before the
// legacy primitive/container fallbacks, so the explicit stdlib mapping wins
// over the legacy nameParts heuristics (e.g. i64 -> std::int64_t even when
// the parser also sets nameParts={"i64"}). Mapping:
//
//   bool        -> bool
//   i64         -> std::int64_t          (requires <cstdint>)
//   f64         -> double
//   string      -> std::string_view      (UTF-8 view, requires <string_view>)
//   optional<T> -> std::optional<T>      (requires <optional>)
//   slice<T>    -> topo::span<const T>   (requires <topo/span.h>)
//   bytes       -> topo::span<const std::uint8_t>
//                                        (slice<u8>-isomorphic; <topo/span.h>+<cstdint>)
//   array<T, N> -> std::array<T, N>      (fixed-length inline; requires <array>)
//
// emit() prepends a conditional `#include` preamble when stdlib types are
// referenced anywhere in the module, mirroring the approach the
// PythonEmitter uses for its `from typing import ...` block.

static std::string ind(int level) {
    return std::string(level * 4, ' ');
}

static std::string fidelityComment(Fidelity f, int level) {
    if (f == Fidelity::Recovered) return ind(level) + "// [recovered]\n";
    if (f == Fidelity::Inferred) return ind(level) + "// [inferred]\n";
    return "";
}

// Render the declaration-site `template <...>` clause for a generic
// struct/function. A bare type-param emits as `typename T`; a type-param
// whose constraintType is populated (single trait-bound MVP) emits as
// `Bound T` — the C++20 constrained-parameter form, which keeps bounds
// per-parameter without a separate `requires` clause and is cleaner than
// the equivalent `template <typename T> requires Bound<T>`. A type-param
// flagged `isVariadic` emits as a parameter pack `typename... Ts`. A
// `TemplateTemplateParam` emits as `template <typename...> class C`, with
// one `typename` per `innerParams` entry. Multi-segment
// bound paths are joined with `::` (e.g. `template <std::integral T>`).
// When defaultType is populated (C++17+) it emits as `... T = X`; multi-
// segment defaults likewise join with `::` (e.g. `std::string`). bound +
// default coexist as `Bound T = X`. Empty list ⇒ "" ⇒ byte-identical to
// pre-generics output (no clause, no trailing newline). Empty bound +
// empty default also stay byte-identical to pre-bounds output.
//
// Multi-bound TypeParams (`extraBounds` non-empty, total bounds ≥ 2) cannot
// ride the constrained-parameter form — C++20 permits only one concept
// before `T` in `<Concept T>`. The whole clause switches to the trailing
// `requires` form: every type-param renders as `typename T` and a
// `requires A<T> && B<T> && ...` tail is appended. The switch happens
// only when at least one param has multi-bound, so single-bound + bare
// declarations stay byte-identical to the pre-multi-bound output.
// C++ has no equivalent to Rust associated-type bindings
// (`Iterator<Item = u8>`). C++20 concepts can carry positional template
// arguments but binding by name to an associated type is not part of the
// surface form. DROP the bindings and append an inline
// `/* TOPO-TRANSPILE: ... */` block comment after the clause so the human-
// facing diff surfaces the loss. Block comments are legal at the locations
// templateClause produces (declaration prefix line + trailing `requires`).
static bool cppTypeNodeHasAssocBindings(const TypeNode& t) {
    return !t.assocBindings.empty();
}
static std::string cppAssocBindingDropNote(const std::string& paramName) {
    return " /* TOPO-TRANSPILE: associated-type bindings on " + paramName +
           " dropped (no C++ equivalent) */";
}

// Rust lifetime bounds (`T: 'a`) ride the wire as TypeNodes whose
// nameParts[0] starts with `'`. C++ has no lifetime concept — these
// entries are silently dropped at every bound iteration. Same rule for
// lifetime params themselves (kind=Lifetime): they're never rendered in
// C++'s `template <...>` clause. No drop comment because lifetime
// annotations are noise for non-Rust hosts.
static bool isWireLifetimeBound(const TypeNode& t) {
    return !t.nameParts.empty() && !t.nameParts[0].empty() &&
           t.nameParts[0][0] == '\'';
}

// True iff this bound TypeNode is a positional `union<...>` — the wire
// shape a Python TypeVar constraint-tuple lowers to. C++ has no anonymous
// untagged-union type usable as a generic bound, so it is dropped (with a
// visible downgrade note, unlike silently-dropped lifetime bounds).
static bool isPositionalUnionBound(const TypeNode& t) {
    return t.nameParts.size() == 1 && t.nameParts[0] == "union";
}
static std::string cppUnionBoundDropNote(const std::string& paramName) {
    return " /* TOPO-TRANSPILE: union<...> bound on " + paramName +
           " dropped (no C++ untagged-union generic bound) */";
}

static std::string templateClause(const std::vector<TemplateParamDecl>& params) {
    // Filter out kind=Lifetime entries up front so the rest of the
    // multi-bound / nontype / default plumbing never sees them.
    std::vector<TemplateParamDecl> filtered;
    filtered.reserve(params.size());
    for (const auto& p : params) {
        if (p.kind == TemplateParamDecl::LifetimeParam) continue;
        filtered.push_back(p);
    }
    if (filtered.empty()) return "";
    // Strip Rust lifetime bound entries (silent — noise for C++) and
    // positional union<...> bounds (Python TypeVar constraint-tuple shape;
    // no C++ generic-bound equivalent, dropped with a downgrade note) from
    // each type param's bound list.
    std::string unionNotes;
    for (auto& p : filtered) {
        if (p.kind != TemplateParamDecl::TypeParam) continue;
        std::vector<TypeNode> all;
        if (!p.constraintType.nameParts.empty()) all.push_back(p.constraintType);
        for (const auto& eb : p.extraBounds) all.push_back(eb);
        std::vector<TypeNode> kept;
        bool unionDropped = false;
        for (const auto& b : all) {
            if (isWireLifetimeBound(b)) continue;
            if (isPositionalUnionBound(b)) { unionDropped = true; continue; }
            kept.push_back(b);
        }
        if (kept.empty()) {
            p.constraintType = TypeNode{};
            p.extraBounds.clear();
        } else {
            p.constraintType = kept.front();
            p.extraBounds.assign(kept.begin() + 1, kept.end());
        }
        if (unionDropped) unionNotes += cppUnionBoundDropNote(p.name);
    }
    const auto& tps = filtered;
    auto renderJoined = [](const std::vector<std::string>& parts) {
        std::string out;
        for (size_t j = 0; j < parts.size(); ++j) {
            if (j > 0) out += "::";
            out += parts[j];
        }
        return out;
    };

    // Detect whether the trailing-requires form is needed. Triggered only
    // when a TypeParam carries `extraBounds` (constraintType + extraBounds
    // collectively ≥ 2 bounds). NonTypeParam never participates — its
    // `constraintType` is the value type, not a bound.
    bool hasMultiBound = false;
    for (const auto& p : tps) {
        if (p.kind == TemplateParamDecl::TypeParam && !p.extraBounds.empty()) {
            hasMultiBound = true;
            break;
        }
    }

    std::string s = "template <";
    // Collect per-param assoc-binding drop notes; appended once at the end
    // of the template clause (after the closing `>`) so the C++ syntactic
    // body of the clause stays well-formed regardless of bound positioning.
    std::string assocNotes;
    auto noteAssocOn = [&](size_t idx) {
        const auto& p = tps[idx];
        bool anyAssoc = cppTypeNodeHasAssocBindings(p.constraintType);
        for (const auto& eb : p.extraBounds) {
            if (cppTypeNodeHasAssocBindings(eb)) { anyAssoc = true; break; }
        }
        if (anyAssoc) assocNotes += cppAssocBindingDropNote(p.name);
    };
    for (size_t i = 0; i < tps.size(); ++i) {
        if (i > 0) s += ", ";
        if (tps[i].kind == TemplateParamDecl::TemplateTemplateParam) {
            // Template-template parameter: `template <typename...> class C`.
            // The MVP extractor only captures the canonical
            // `template<typename> class X` shape, so the inner clause is
            // simply `<typename>` repeated per innerParams entry (each inner
            // param is a plain type param). An empty innerParams list
            // (defensive) still emits a well-formed `template <> class C`.
            s += "template <";
            for (size_t k = 0; k < tps[i].innerParams.size(); ++k) {
                if (k > 0) s += ", ";
                s += "typename";
                if (tps[i].innerParams[k].isVariadic) s += "...";
            }
            s += "> class " + tps[i].name;
        } else if (tps[i].kind == TemplateParamDecl::NonTypeParam &&
            !tps[i].constraintType.nameParts.empty()) {
            // `Type N` — non-type template parameter (`template <int N>`,
            // `template <std::size_t N>`). The constraintType carries the
            // value type; multi-segment paths join with `::`.
            s += renderJoined(tps[i].constraintType.nameParts);
            s += " " + tps[i].name;
        } else if (!hasMultiBound &&
                   tps[i].kind == TemplateParamDecl::TypeParam &&
                   !tps[i].constraintType.nameParts.empty()) {
            // `Bound T` (C++20 constrained-parameter form). Only used when
            // the whole clause stays single-bound; with any multi-bound
            // present we switch every type-param to plain `typename T` and
            // collect bounds in the trailing requires clause.
            s += renderJoined(tps[i].constraintType.nameParts);
            // A constrained variadic (`Concept... Ts`) is outside the MVP
            // extractor's range, but render it correctly if a model carries
            // both: the pack ellipsis follows the concept name.
            if (tps[i].isVariadic) s += "...";
            s += " " + tps[i].name;
            noteAssocOn(i);
        } else {
            // Plain `typename T`, or a variadic pack `typename... Ts`.
            s += "typename";
            if (tps[i].kind == TemplateParamDecl::TypeParam &&
                tps[i].isVariadic) {
                s += "...";
            }
            s += " " + tps[i].name;
            if (tps[i].kind == TemplateParamDecl::TypeParam) {
                noteAssocOn(i);
            }
        }
        if (tps[i].kind == TemplateParamDecl::TypeParam &&
            tps[i].defaultType.has_value() &&
            !tps[i].defaultType->nameParts.empty()) {
            s += " = " + renderJoined(tps[i].defaultType->nameParts);
        }
        // NonTypeParam default literal (`template <int N = 10>`). The wire
        // carries the source spelling verbatim — integer / bool / enum
        // literal — so we emit it as-is without re-formatting. Omit-when-
        // empty preserves byte-identical output for nontype params without
        // a default.
        if (tps[i].kind == TemplateParamDecl::NonTypeParam &&
            tps[i].defaultValue.has_value() &&
            !tps[i].defaultValue->empty()) {
            s += " = " + *tps[i].defaultValue;
        }
    }
    s += ">";
    s += assocNotes;
    s += unionNotes;

    if (hasMultiBound) {
        // Build the trailing `requires A<T> && B<T> ...` clause. Walk each
        // TypeParam and emit `Concept<T>` for constraintType followed by
        // each entry in extraBounds. Joined with ` && `.
        std::string req;
        bool first = true;
        auto emitConcept = [&](const std::vector<std::string>& parts,
                                const std::string& tName) {
            if (parts.empty()) return;
            if (!first) req += " && ";
            req += renderJoined(parts);
            req += "<" + tName + ">";
            first = false;
        };
        for (const auto& p : tps) {
            if (p.kind != TemplateParamDecl::TypeParam) continue;
            emitConcept(p.constraintType.nameParts, p.name);
            for (const auto& eb : p.extraBounds) {
                emitConcept(eb.nameParts, p.name);
            }
        }
        if (!req.empty()) {
            s += " requires " + req;
        }
    }

    s += "\n";
    return s;
}

static std::string binaryOpStr(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Mod: return "%";
    case BinaryOp::Eq: return "==";
    case BinaryOp::NotEq: return "!=";
    case BinaryOp::Less: return "<";
    case BinaryOp::Greater: return ">";
    case BinaryOp::LessEq: return "<=";
    case BinaryOp::GreaterEq: return ">=";
    case BinaryOp::And: return "&&";
    case BinaryOp::Or: return "||";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";
    }
    return "??";
}

static std::string mapConcreteType(const std::string& name) {
    // Rust integer types -> C++
    if (name == "i32") return "int";
    if (name == "i64") return "int64_t";
    if (name == "i16") return "int16_t";
    if (name == "i8") return "int8_t";
    if (name == "u32") return "uint32_t";
    if (name == "u64") return "uint64_t";
    if (name == "u16") return "uint16_t";
    if (name == "u8") return "uint8_t";
    if (name == "usize") return "size_t";
    if (name == "isize") return "ptrdiff_t";
    // Rust/Java float types
    if (name == "f64") return "double";
    if (name == "f32") return "float";
    // Python/Java boolean
    if (name == "boolean") return "bool";
    // Rust/Python string
    if (name == "String" || name == "str") return "std::string";
    // Void
    if (name == "Void" || name == "None") return "void";
    return "";
}

static std::string mapContainerType(const std::string& name) {
    if (name == "Vec" || name == "List" || name == "ArrayList" || name == "list") return "std::vector";
    if (name == "Option" || name == "Optional") return "std::optional";
    if (name == "HashMap" || name == "Map" || name == "dict") return "std::unordered_map";
    if (name == "HashSet" || name == "Set" || name == "set") return "std::unordered_set";
    return "";
}

static std::pair<std::string, std::string> splitQualifiedName(const std::string& qname) {
    auto pos = qname.rfind("::");
    if (pos == std::string::npos)
        return {"", qname};
    return {qname.substr(0, pos), qname.substr(pos + 2)};
}

CppEmitter::CppEmitter(TypeBinder binder) : binder_(std::move(binder)) {}

// Scan a TypeNode (and its templateArgs recursively) for
// any stdlib bridging type that requires a host header. Sets the matching
// flags so emit() can prepend the right `#include` directives.
namespace {
struct StdlibIncludeFlags {
    bool needsCstdint = false;     // std::int64_t — i64
    bool needsStringView = false;  // std::string_view — string
    bool needsOptional = false;    // std::optional — optional<T>
    bool needsTopoSpan = false;    // topo::span — slice<T> / bytes
    bool needsArray = false;       // std::array — array<T, N>
    bool needsTuple = false;       // std::tuple — record<...>
};

void scanTypeForStdlibIncludes(const TypeNode& t, StdlibIncludeFlags& flags) {
    if (t.isStdlib()) {
        switch (t.stdlibId) {
        case stdlib::TypeId::I64:      flags.needsCstdint = true; break;
        case stdlib::TypeId::TimeNs:   flags.needsCstdint = true; break; // i64-isomorphic
        case stdlib::TypeId::String:   flags.needsStringView = true; break;
        case stdlib::TypeId::Optional: flags.needsOptional = true; break;
        case stdlib::TypeId::Slice:    flags.needsTopoSpan = true; break;
        case stdlib::TypeId::Bytes:
            // bytes is slice<u8>-isomorphic: same topo::span host type,
            // and the u8 element renders as std::uint8_t (needs <cstdint>).
            flags.needsTopoSpan = true;
            flags.needsCstdint = true;
            break;
        case stdlib::TypeId::Array:
            // array<T, N> -> std::array<T, N>; element T recursion handled
            // by the templateArgs loop below.
            flags.needsArray = true;
            break;
        case stdlib::TypeId::Uuid:
        case stdlib::TypeId::Decimal128:
            // uuid / decimal128 -> std::array<std::uint8_t, 16>: a fixed
            // 16-byte buffer (C++ has no native UUID or IEEE 754 decimal128).
            // Needs both <array> and <cstdint>.
            flags.needsArray = true;
            flags.needsCstdint = true;
            break;
        case stdlib::TypeId::Record:
        case stdlib::TypeId::Union:
            // record<...> / union<...> -> std::tuple<...>; field-type
            // recursion handled by the recordFields loop below. union shares
            // record's positional-tuple surface — the variant overlap is a
            // byte-contract fact the .topo declaration owns, not the host
            // type.
            flags.needsTuple = true;
            break;
        // Width-extension integers all use std::*int*_t.
        case stdlib::TypeId::U8:
        case stdlib::TypeId::I32:
        case stdlib::TypeId::U32:
        case stdlib::TypeId::U64:
        case stdlib::TypeId::I8:
        case stdlib::TypeId::I16:
        case stdlib::TypeId::U16:
            flags.needsCstdint = true; break;
        case stdlib::TypeId::Bool:
        case stdlib::TypeId::F64:
        case stdlib::TypeId::F32:
        case stdlib::TypeId::None:
            break; // builtin / sentinel — no include
        }
    }
    for (const auto& arg : t.templateArgs)
        scanTypeForStdlibIncludes(arg, flags);
    // record<...> holds its field types in recordFields, not templateArgs;
    // recurse them so a nested field (e.g. record<x: string>) still pulls
    // in its own header.
    for (const auto& f : t.recordFields)
        scanTypeForStdlibIncludes(f.type(), flags);
}
} // namespace

EmitResult CppEmitter::emit(const TranspileModule& module) {
    EmitResult result;

    // Walk the module to collect stdlib-related headers
    // that must be `#include`d in the generated C++ before any namespace
    // wrapping. Only emits a header when the corresponding stdlib type is
    // actually referenced, so legacy modules stay free of preamble noise.
    StdlibIncludeFlags incFlags;
    auto scanFn = [&](const TranspileFunction& f) {
        scanTypeForStdlibIncludes(f.returnType, incFlags);
        for (const auto& p : f.params)
            scanTypeForStdlibIncludes(p.type, incFlags);
    };
    for (const auto& f : module.functions) scanFn(f);
    for (const auto& t : module.types) {
        for (const auto& field : t.fields)
            scanTypeForStdlibIncludes(field.type, incFlags);
    }

    std::string preamble;
    if (incFlags.needsCstdint)    preamble += "#include <cstdint>\n";
    if (incFlags.needsOptional)   preamble += "#include <optional>\n";
    if (incFlags.needsStringView) preamble += "#include <string_view>\n";
    if (incFlags.needsArray)      preamble += "#include <array>\n";
    if (incFlags.needsTuple)      preamble += "#include <tuple>\n";
    if (incFlags.needsTopoSpan)   preamble += "#include <topo/span.h>\n";
    if (!preamble.empty()) preamble += "\n";
    result.code += preamble;

    struct NsGroup {
        std::vector<const TranspileType*> types;
        std::vector<const TranspileFunction*> functions;
    };
    std::map<std::string, NsGroup> groups;

    for (const auto& t : module.types) {
        auto [ns, _] = splitQualifiedName(t.qualifiedName);
        groups[ns].types.push_back(&t);
    }
    for (const auto& f : module.functions) {
        auto [ns, _] = splitQualifiedName(f.qualifiedName);
        groups[ns].functions.push_back(&f);
    }

    for (const auto& [ns, group] : groups) {
        std::vector<std::string> nsParts;
        if (!ns.empty()) {
            size_t start = 0;
            while (start < ns.size()) {
                auto next = ns.find("::", start);
                if (next == std::string::npos) {
                    nsParts.push_back(ns.substr(start));
                    break;
                }
                nsParts.push_back(ns.substr(start, next - start));
                start = next + 2;
            }
            for (const auto& part : nsParts)
                result.code += "namespace " + part + " {\n";
        }

        for (const auto* t : group.types)
            result.code += emitStruct(*t) + "\n";
        for (const auto* f : group.functions)
            result.code += emitFunction(*f) + "\n";

        if (!ns.empty()) {
            for (size_t i = nsParts.size(); i > 0; --i)
                result.code += "} // namespace " + nsParts[i - 1] + "\n";
        }
    }

    return result;
}

std::string CppEmitter::emitOwnership(const TypeNode& type) {
    // Copy-and-mutate, not positional reconstruction: a positional
    // TypeNode{...} silently drops any field not listed (stdlibId,
    // recordFields), so `owned slice<T>` / `owned record<...>` would lose
    // their stdlib identity through the ownership path.
    TypeNode bare = type;
    bare.ownership = OwnershipKind::None;
    bare.modifier = TypeNode::None;
    std::string inner = emitType(bare);

    switch (type.ownership) {
    case OwnershipKind::Owned: return "std::unique_ptr<" + inner + ">";
    case OwnershipKind::Shared: return "std::shared_ptr<" + inner + ">";
    case OwnershipKind::Weak: return "std::weak_ptr<" + inner + ">";
    case OwnershipKind::None: break;
    }
    return inner;
}

std::string CppEmitter::emitType(const TypeNode& type) {
    if (type.ownership != OwnershipKind::None) return emitOwnership(type);

    // Stdlib bridging types route through this branch
    // BEFORE TypeBinder / primitive / container fallbacks, so the explicit
    // stdlib mapping wins over the legacy nameParts-based heuristics. The 6
    // first-batch types all map onto C++17 std or topo:: header types — no
    // runtime helper required.
    if (type.isStdlib()) {
        std::string mapped;
        switch (type.stdlibId) {
        case stdlib::TypeId::Bool:   mapped = "bool"; break;
        case stdlib::TypeId::I64:    mapped = "std::int64_t"; break;
        case stdlib::TypeId::TimeNs: mapped = "std::int64_t"; break; // ns since epoch, i64-isomorphic
        case stdlib::TypeId::Uuid:   mapped = "std::array<std::uint8_t, 16>"; break; // 16-byte RFC 4122 buffer
        case stdlib::TypeId::Decimal128: mapped = "std::array<std::uint8_t, 16>"; break; // 16-byte IEEE 754-2008 buffer
        case stdlib::TypeId::F64:    mapped = "double"; break;
        // Width-extension scalars; all <cstdint> /
        // built-in C++ types, no helper needed.
        case stdlib::TypeId::U8:     mapped = "std::uint8_t"; break;
        case stdlib::TypeId::I32:    mapped = "std::int32_t"; break;
        case stdlib::TypeId::U32:    mapped = "std::uint32_t"; break;
        case stdlib::TypeId::U64:    mapped = "std::uint64_t"; break;
        case stdlib::TypeId::F32:    mapped = "float"; break;
        case stdlib::TypeId::I8:     mapped = "std::int8_t"; break;
        case stdlib::TypeId::I16:    mapped = "std::int16_t"; break;
        case stdlib::TypeId::U16:    mapped = "std::uint16_t"; break;
        case stdlib::TypeId::String:
            // UTF-8, non-owning view — layout
            // ({u32 len_bytes, u8* utf8_ptr}). string_view matches that
            // contract on the host side without imposing allocation.
            mapped = "std::string_view";
            break;
        case stdlib::TypeId::Optional: {
            // Sema rejects optional<> upstream; the fallback keeps the
            // emitter total in defensive contexts (round-trip tests etc.).
            std::string inner = type.templateArgs.empty()
                ? std::string("void")
                : emitType(type.templateArgs[0]);
            mapped = "std::optional<" + inner + ">";
            break;
        }
        case stdlib::TypeId::Slice: {
            std::string inner = type.templateArgs.empty()
                ? std::string("void")
                : emitType(type.templateArgs[0]);
            mapped = "topo::span<const " + inner + ">";
            break;
        }
        case stdlib::TypeId::Bytes: {
            // bytes is slice<u8>-isomorphic: emit the exact same host type
            // the Slice case produces for a u8 element. The element keyword
            // is fixed (no templateArgs on bytes), so synthesize the u8
            // TypeNode and route it through emitType, mirroring how Slice
            // recurses into its element. Result: topo::span<const std::uint8_t>.
            TypeNode u8;
            u8.nameParts = {stdlib::keywordOf(stdlib::TypeId::U8)};
            u8.stdlibId = stdlib::TypeId::U8;
            std::string inner = emitType(u8);
            mapped = "topo::span<const " + inner + ">";
            break;
        }
        case stdlib::TypeId::Array: {
            // array<T, N> -> std::array<T, N>: fixed-length inline buffer.
            // T is templateArgs[0] (recurse, like Slice/Optional); N is the
            // compile-time integer in templateArgs[1].nonTypeValue.
            std::string inner = type.templateArgs.empty()
                ? std::string("void")
                : emitType(type.templateArgs[0]);
            std::string n = "0";
            if (type.templateArgs.size() >= 2 &&
                type.templateArgs[1].nonTypeValue.has_value())
                n = std::to_string(*type.templateArgs[1].nonTypeValue);
            mapped = "std::array<" + inner + ", " + n + ">";
            break;
        }
        case stdlib::TypeId::Record: {
            // record<f1: T1, f2: T2, ...> -> std::tuple<T1, T2, ...>.
            // Field order is the load-bearing cross-language byte contract;
            // field names live in the .topo declaration, not the host type
            // (same positional idiom PythonEmitter uses for record). Each
            // field type recurses, so nested stdlib types compose.
            std::string out = "std::tuple<";
            for (size_t i = 0; i < type.recordFields.size(); ++i) {
                if (i > 0) out += ", ";
                out += emitType(type.recordFields[i].type());
            }
            out += ">";
            mapped = out;
            break;
        }
        case stdlib::TypeId::Union: {
            // union<tag: TagT, v1: T1, ...> -> std::tuple<TagT, T1, ...>.
            // C++ has no anonymous tagged-union literal that pins a byte
            // layout; the order-preserving tuple is the faithful surface,
            // the same idiom record uses. The .topo declaration owns the
            // field names and the variant-overlap layout (tag first, only
            // the tag-selected variant occupying the shared storage); the
            // tuple necessarily widens to carry tag plus every variant slot.
            std::string out = "std::tuple<";
            for (size_t i = 0; i < type.recordFields.size(); ++i) {
                if (i > 0) out += ", ";
                out += emitType(type.recordFields[i].type());
            }
            out += ">";
            mapped = out;
            break;
        }
        case stdlib::TypeId::None:
            break; // fall through to legacy paths
        }
        if (!mapped.empty()) {
            std::string result;
            if (type.isConst) result += "const ";
            result += mapped;
            if (type.modifier == TypeNode::Ref) result += "&";
            else if (type.modifier == TypeNode::Ptr) result += "*";
            return result;
        }
    }

    // Try TypeBinder resolution for single-part abstract names
    if (type.nameParts.size() == 1) {
        auto resolved = binder_.resolve(type.nameParts[0]);
        if (resolved) {
            std::string result;
            if (type.isConst) result += "const ";
            result += *resolved;
            if (type.modifier == TypeNode::Ref)
                result += "&";
            else if (type.modifier == TypeNode::Ptr)
                result += "*";
            return result;
        }
    }

    // Try concrete source-language type mapping
    if (type.nameParts.size() == 1) {
        auto mapped = mapConcreteType(type.nameParts[0]);
        if (!mapped.empty()) {
            std::string result;
            if (type.isConst) result += "const ";
            result += mapped;
            if (type.modifier == TypeNode::Ref) result += "&";
            else if (type.modifier == TypeNode::Ptr) result += "*";
            return result;
        }
        auto container = mapContainerType(type.nameParts[0]);
        if (!container.empty()) {
            std::string result;
            if (type.isConst) result += "const ";
            result += container;
            if (!type.templateArgs.empty()) {
                result += "<";
                for (size_t i = 0; i < type.templateArgs.size(); ++i) {
                    if (i > 0) result += ", ";
                    result += emitType(type.templateArgs[i]);
                }
                result += ">";
            }
            if (type.modifier == TypeNode::Ref) result += "&";
            else if (type.modifier == TypeNode::Ptr) result += "*";
            return result;
        }
    }

    std::string result;
    if (type.isConst) result += "const ";

    for (size_t i = 0; i < type.nameParts.size(); ++i) {
        if (i > 0) result += "::";
        result += type.nameParts[i];
    }

    if (!type.templateArgs.empty()) {
        result += "<";
        for (size_t i = 0; i < type.templateArgs.size(); ++i) {
            if (i > 0) result += ", ";
            result += emitType(type.templateArgs[i]);
        }
        result += ">";
    }

    if (type.modifier == TypeNode::Ref)
        result += "&";
    else if (type.modifier == TypeNode::Ptr)
        result += "*";

    return result;
}

std::string CppEmitter::emitExpr(const Expr& expr) {
    switch (expr.kind()) {
    case Expr::Kind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        return "(" + emitExpr(*e.lhs) + " " + binaryOpStr(e.op) + " " + emitExpr(*e.rhs) + ")";
    }
    case Expr::Kind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        std::string op;
        switch (e.op) {
        case UnaryOp::Negate: op = "-"; break;
        case UnaryOp::Not: op = "!"; break;
        case UnaryOp::BitNot: op = "~"; break;
        case UnaryOp::PreIncrement: return "++" + emitExpr(*e.operand);
        case UnaryOp::PostIncrement: return emitExpr(*e.operand) + "++";
        case UnaryOp::PreDecrement: return "--" + emitExpr(*e.operand);
        case UnaryOp::PostDecrement: return emitExpr(*e.operand) + "--";
        }
        return op + emitExpr(*e.operand);
    }
    case Expr::Kind::Call: {
        const auto& e = static_cast<const CallExpr&>(expr);
        std::string result = e.callee + "(";
        for (size_t i = 0; i < e.args.size(); ++i) {
            if (i > 0) result += ", ";
            result += emitExpr(*e.args[i]);
        }
        result += ")";
        return result;
    }
    case Expr::Kind::MemberAccess: {
        const auto& e = static_cast<const MemberAccessExpr&>(expr);
        return emitExpr(*e.object) + "." + e.member;
    }
    case Expr::Kind::Index: {
        const auto& e = static_cast<const IndexExpr&>(expr);
        return emitExpr(*e.object) + "[" + emitExpr(*e.index) + "]";
    }
    case Expr::Kind::Literal: {
        const auto& e = static_cast<const LiteralExpr&>(expr);
        if (e.litKind == LiteralKind::String) return "\"" + e.value + "\"";
        if (e.litKind == LiteralKind::Boolean) return (e.value == "true") ? "true" : "false";
        return e.value;
    }
    case Expr::Kind::VarRef: {
        const auto& e = static_cast<const VarRefExpr&>(expr);
        return e.name;
    }
    case Expr::Kind::Construct: {
        const auto& e = static_cast<const ConstructExpr&>(expr);
        std::string result = emitType(e.type) + "{";
        for (size_t i = 0; i < e.args.size(); ++i) {
            if (i > 0) result += ", ";
            result += emitExpr(*e.args[i]);
        }
        result += "}";
        return result;
    }
    case Expr::Kind::Lambda: {
        const auto& e = static_cast<const LambdaExpr&>(expr);
        std::string result = "[";
        for (size_t i = 0; i < e.captures.size(); ++i) {
            if (i > 0) result += ", ";
            if (e.captures[i].mode == CaptureMode::ByReference) result += "&";
            result += e.captures[i].name;
        }
        result += "](";
        for (size_t i = 0; i < e.params.size(); ++i) {
            if (i > 0) result += ", ";
            result += emitType(e.params[i].type) + " " + e.params[i].name;
        }
        result += ")";
        if (!e.returnType.nameParts.empty()) result += " -> " + emitType(e.returnType);
        result += " {\n";
        for (const auto& st : e.body)
            result += emitStmt(*st, 1);
        result += "}";
        return result;
    }
    case Expr::Kind::Throw: {
        const auto& e = static_cast<const ThrowExpr&>(expr);
        return "throw " + emitExpr(*e.operand);
    }
    case Expr::Kind::Unsupported: {
        const auto& e = static_cast<const UnsupportedExpr&>(expr);
        return "/* TOPO-TRANSPILE: unsupported — " + e.description + " */";
    }
    case Expr::Kind::Ternary: {
        const auto& e = static_cast<const TernaryExpr&>(expr);
        return "(" + emitExpr(*e.condition) + " ? " + emitExpr(*e.trueExpr) + " : " + emitExpr(*e.falseExpr) + ")";
    }
    case Expr::Kind::CompoundAssign: {
        const auto& e = static_cast<const CompoundAssignExpr&>(expr);
        return emitExpr(*e.target) + " " + binaryOpStr(e.op) + "= " + emitExpr(*e.value);
    }
    }
    return "/* TOPO-TRANSPILE: unsupported — unknown expression */";
}

std::string CppEmitter::emitStmt(const Stmt& stmt, int level) {
    std::string prefix = fidelityComment(stmt.fidelity, level);

    switch (stmt.kind()) {
    case Stmt::Kind::VarDecl: {
        const auto& s = static_cast<const VarDeclStmt&>(stmt);
        std::string result = prefix + ind(level) + emitType(s.type) + " " + s.name;
        if (s.init) result += " = " + emitExpr(*s.init);
        result += ";\n";
        return result;
    }
    case Stmt::Kind::Assign: {
        const auto& s = static_cast<const AssignStmt&>(stmt);
        return prefix + ind(level) + emitExpr(*s.target) + " = " + emitExpr(*s.value) + ";\n";
    }
    case Stmt::Kind::Return: {
        const auto& s = static_cast<const ReturnStmt&>(stmt);
        if (s.value) return prefix + ind(level) + "return " + emitExpr(*s.value) + ";\n";
        return prefix + ind(level) + "return;\n";
    }
    case Stmt::Kind::If: {
        const auto& s = static_cast<const IfStmt&>(stmt);
        std::string result = prefix + ind(level) + "if (" + emitExpr(*s.condition) + ") {\n";
        for (const auto& st : s.thenBody)
            result += emitStmt(*st, level + 1);
        result += ind(level) + "}";
        if (!s.elseBody.empty()) {
            result += " else {\n";
            for (const auto& st : s.elseBody)
                result += emitStmt(*st, level + 1);
            result += ind(level) + "}";
        }
        result += "\n";
        return result;
    }
    case Stmt::Kind::For: {
        const auto& s = static_cast<const ForStmt&>(stmt);
        std::string init;
        if (s.init) {
            // Emit the init statement inline (strip trailing ";\n")
            std::string raw = emitStmt(*s.init, 0);
            // Remove leading whitespace and trailing ";\n"
            size_t start = raw.find_first_not_of(" \t\n");
            if (start != std::string::npos) raw = raw.substr(start);
            while (!raw.empty() && (raw.back() == '\n' || raw.back() == ';' || raw.back() == ' '))
                raw.pop_back();
            init = raw;
        }
        std::string cond = s.condition ? emitExpr(*s.condition) : "";
        std::string incr = s.increment ? emitExpr(*s.increment) : "";

        std::string result = prefix + ind(level) + "for (" + init + "; " + cond + "; " + incr + ") {\n";
        for (const auto& st : s.body)
            result += emitStmt(*st, level + 1);
        result += ind(level) + "}\n";
        return result;
    }
    case Stmt::Kind::While: {
        const auto& s = static_cast<const WhileStmt&>(stmt);
        std::string result = prefix + ind(level) + "while (" + emitExpr(*s.condition) + ") {\n";
        for (const auto& st : s.body)
            result += emitStmt(*st, level + 1);
        result += ind(level) + "}\n";
        return result;
    }
    case Stmt::Kind::ExprStmt: {
        const auto& s = static_cast<const ExprStmt&>(stmt);
        return prefix + ind(level) + emitExpr(*s.expr) + ";\n";
    }
    case Stmt::Kind::TryCatch: {
        const auto& s = static_cast<const TryCatchStmt&>(stmt);
        std::string result = prefix + ind(level) + "try {\n";
        for (const auto& st : s.tryBody)
            result += emitStmt(*st, level + 1);
        result += ind(level) + "}";
        for (const auto& c : s.catchClauses) {
            result += " catch (" + emitType(c.exceptionType);
            if (!c.varName.empty()) result += "& " + c.varName;
            result += ") {\n";
            for (const auto& st : c.body)
                result += emitStmt(*st, level + 1);
            result += ind(level) + "}";
        }
        if (!s.finallyBody.empty()) {
            result += " // finally (C++ has no finally; use RAII)\n";
            for (const auto& st : s.finallyBody)
                result += emitStmt(*st, level);
        }
        result += "\n";
        return result;
    }
    case Stmt::Kind::Break: return prefix + ind(level) + "break;\n";
    case Stmt::Kind::Continue: return prefix + ind(level) + "continue;\n";
    case Stmt::Kind::Switch: {
        const auto& s = static_cast<const SwitchStmt&>(stmt);
        std::string result = prefix + ind(level) + "switch (" + emitExpr(*s.subject) + ") {\n";
        for (const auto& c : s.cases) {
            if (c.value)
                result += ind(level) + "case " + emitExpr(*c.value) + ":\n";
            else
                result += ind(level) + "default:\n";
            for (const auto& st : c.body)
                result += emitStmt(*st, level + 1);
        }
        result += ind(level) + "}\n";
        return result;
    }
    }
    return prefix + ind(level) + "// TOPO-TRANSPILE: unsupported — unknown statement\n";
}

std::string CppEmitter::emitFunction(const TranspileFunction& func) {
    std::string result;
    result += fidelityComment(func.fidelity, 0);

    for (const auto& u : func.unsupported)
        result += "// TOPO-TRANSPILE: unsupported — " + u + "\n";

    result += templateClause(func.templateParams);

    auto [_, simpleName] = splitQualifiedName(func.qualifiedName);
    result += emitType(func.returnType) + " " + simpleName + "(";
    for (size_t i = 0; i < func.params.size(); ++i) {
        if (i > 0) result += ", ";
        result += emitType(func.params[i].type) + " " + func.params[i].name;
    }
    result += ") {\n";

    for (const auto& s : func.body)
        result += emitStmt(*s, 1);

    result += "}\n";
    return result;
}

std::string CppEmitter::emitStruct(const TranspileType& type) {
    std::string result;
    result += fidelityComment(type.fidelity, 0);
    result += templateClause(type.templateParams);
    auto [_, simpleName] = splitQualifiedName(type.qualifiedName);
    result += "struct " + simpleName;

    // Inheritance hierarchy, source order. C++ has no class/interface
    // distinction, so baseClassKinds is irrelevant — every base is `public`.
    // Empty baseClasses ⇒ no base-clause, byte-identical to pre-inheritance.
    if (!type.baseClasses.empty()) {
        result += " : ";
        for (size_t i = 0; i < type.baseClasses.size(); ++i) {
            if (i > 0) result += ", ";
            result += "public " + emitType(type.baseClasses[i]);
        }
    }
    result += " {\n";

    for (const auto& f : type.fields) {
        result += fidelityComment(f.fidelity, 1);
        result += ind(1) + emitType(f.type) + " " + f.name + ";\n";
    }

    result += "};\n";
    return result;
}

} // namespace topo::transpile
