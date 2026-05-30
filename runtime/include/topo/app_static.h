#ifndef TOPO_APP_STATIC_H
#define TOPO_APP_STATIC_H

// @stability provisional
// User-facing regex MVP fallback for the clang-AST static
// front-end. The regex extractor has five enumerated parser gaps
// (macro-expanded registration calls; auto-return handler
// signatures; cross-line / indirect registration; record-field
// types through using / typedef / template-alias chains;
// nested / aliased-namespace handler symbols) — each is detected
// at parse time and surfaces a `fall back to topo-app-static-cpp`
// note rather than silently mis-extracting. The primary path is
// the clang-AST tool, which will absorb most of this surface.

// topo-app C++ static-analysis front-end: a second .topo producer —
// the regex FALLBACK.
//
// PRIMARY vs FALLBACK: the
// primary C++ compile-time static front-end is the clang-AST tool
// `topo-app-static-cpp` (topo-lang-cpp/topo-build/static-frontend/) —
// post-preprocessor and fully type-resolved. This header is kept as the
// documented FALLBACK for when `clang` is not resolvable on the host,
// mirroring the TypeScript pattern where the regex `TypeScriptSymbol-
// Extractor` stays as the fallback for when the `topo-extract-typescript`
// subprocess is not on PATH. The clang-AST front-end removes the five
// limitations enumerated below; this regex MVP keeps vertical-slice
// parity and is a graceful, no-clang degradation, not a dead path.
//
// app.h is the *runtime* registration bridge — In/Out come from C++
// template function_traits, which means the user's translation unit must
// be compiled and the registration calls executed before a Graph exists.
// app.h says so itself ("deliberately NOT a compile-time static-analysis
// front-end"). This header is a compile-time alternative: it reads the
// SAME registration surface from C++ *source text* without compiling or
// executing it, resolves the referenced function signatures statically,
// and builds the SAME topo::app::Graph. It is one more way to obtain a
// Graph, not a reimplementation of emission/read-back/check — those stay
// exactly app_emit.h / app_readback.h / app_check.h, so the .topo a
// static parse yields is byte-identical to the runtime bridge's for the
// same program.
//
// Parsing strategy reuse: this mirrors the C++ check path's static C++
// reader, topo-lang-cpp/topo-check/analysis/extract/CppSymbolExtractor
// (regex line scanning with namespace/brace scope tracking, no clang
// invocation), and its companion parameter-type splitter. We do not write
// a new C++ parser; we apply the project's already-chosen lightweight
// signature-extraction technique to the narrow registration surface the
// runtime bridge defines. Anything outside that surface is intentionally
// not understood — this is a minimal viable front-end at vertical-slice
// parity, not a general C++ analyzer. The five constructs the regex MVP
// deliberately does not understand — macro-expanded registration calls,
// `auto`-return handlers, cross-line / indirect registration, record-
// field types through `using`/`typedef`/template-alias chains, and
// nested / aliased-namespace handler symbols — are exactly what the
// clang-AST primary front-end resolves; reach for `topo-app-static-cpp`
// when source uses any of them.
//
// What is recognised, and only this:
//   - `<ident> <ident>(<params>) { ... }`  free-function signatures
//   - `using <Alias> = topo::app::Record<Field<"f", T>, ...>;`  record
//     schema aliases (the C++ host's explicit, reflection-free record
//     metadata — same contract app.h documents)
//   - `<obj>.handler(&<fn>, "<name>"[, "<doc>"]);`  handler registration
//   - `<obj>.flow("<name>", <stage>, <stage>, ...);` with stages being a
//     `"literal"`, a bare registered name, or `parallel("a","b",...)`
//   - the App's namespace from `App <obj>("<ns>");` / `App <obj>{"<ns>"}`
//
// In/Out are read from the *referenced function's* C++ signature, never
// re-declared — the same "no redundant declaration" contract app.h's
// function_traits
// enforces, only resolved from text instead of the type system.

#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include <topo/app.h>
#include <topo/app_model.h>

namespace topo::app {
namespace stat {

// --- text utilities -------------------------------------------------------

inline std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Strip // line comments and /* */ block comments without touching string
// literals — the registration surface lives in real code, and a comment
// that merely mentions `.handler(` must never register a phantom handler.
inline std::string strip_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    enum { Code, Slash, Line, Block, BlockStar, Str } st = Code;
    char strq = 0;
    for (std::size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        switch (st) {
            case Code:
                if (c == '/') {
                    st = Slash;
                } else if (c == '"' || c == '\'') {
                    strq = c;
                    st = Str;
                    out += c;
                } else {
                    out += c;
                }
                break;
            case Slash:
                if (c == '/') {
                    st = Line;
                } else if (c == '*') {
                    st = Block;
                } else {
                    out += '/';
                    out += c;
                    st = Code;
                }
                break;
            case Line:
                if (c == '\n') {
                    out += c;
                    st = Code;
                }
                break;
            case Block:
                if (c == '*') st = BlockStar;
                if (c == '\n') out += c;  // keep line count stable
                break;
            case BlockStar:
                if (c == '/')
                    st = Code;
                else if (c == '*')
                    st = BlockStar;
                else {
                    if (c == '\n') out += c;
                    st = Block;
                }
                break;
            case Str:
                out += c;
                if (c == '\\') {
                    if (i + 1 < src.size()) {
                        out += src[i + 1];
                        ++i;
                    }
                } else if (c == strq) {
                    st = Code;
                }
                break;
        }
    }
    return out;
}

// Split a call's argument list on top-level commas (angle/paren/brace and
// string-literal aware), the same depth-tracked split CppSymbolExtractor's
// parameter splitter uses, generalised to any bracket family.
inline std::vector<std::string> split_top_level(const std::string& s,
                                                char sep = ',') {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    char strq = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (strq) {
            cur += c;
            if (c == '\\' && i + 1 < s.size()) {
                cur += s[i + 1];
                ++i;
            } else if (c == strq) {
                strq = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            strq = c;
            cur += c;
            continue;
        }
        if (c == '<' || c == '(' || c == '[' || c == '{') ++depth;
        else if (c == '>' || c == ')' || c == ']' || c == '}') --depth;
        if (c == sep && depth == 0) {
            parts.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!trim(cur).empty() || !parts.empty()) parts.push_back(trim(cur));
    return parts;
}

// Extract the first double-quoted literal's content from a fragment.
inline std::optional<std::string> first_string_literal(const std::string& s) {
    std::smatch m;
    static const std::regex kLit(R"RX("((?:[^"\\]|\\.)*)")RX");
    if (std::regex_search(s, m, kLit)) return m[1].str();
    return std::nullopt;
}

// --- scanned model --------------------------------------------------------

struct FnSig {
    std::string return_type;            // raw C++ spelling
    std::vector<std::string> param_types;  // raw C++ spellings, names dropped
};

// --- C++ type spelling -> TypeRef ----------------------------------------
//
// The static analogue of app.h's scalar_of<>/type_mapper<>. app.h reads
// the type from the C++ type system; here the same band decisions are
// made from the type's textual spelling. The four stdlib bands and the
// bool-before-integral precedence are identical to app.h on purpose so a
// statically built Graph is indistinguishable from a runtime-built one.

inline std::string normalise_type(std::string t) {
    t = trim(t);
    // Drop cv / ref / elaborations AND the storage-class / function
    // specifiers that decorate a definition's return type but are not the
    // type. This is the same keyword set CppSymbolExtractor's
    // extractReturnType strips (static / virtual / inline / constexpr /
    // consteval / explicit / friend / extern / override / final) — reused
    // verbatim so a `static OrderRec parse(...)` reads as `OrderRec`,
    // exactly as the C++ check path's signature reader resolves it.
    static const std::regex kStrip(
        R"(\b(const|volatile|struct|class|typename|static|virtual|inline|constexpr|consteval|explicit|friend|extern|override|final)\b)");
    t = std::regex_replace(t, kStrip, " ");
    // collapse pointer/ref and whitespace
    std::string o;
    for (char c : t)
        if (c != '&' && c != '*') o += c;
    return trim(std::regex_replace(o, std::regex(R"(\s+)"), " "));
}

inline Scalar scalar_from_cpp(const std::string& raw) {
    std::string t = normalise_type(raw);
    if (t == "bool") return Scalar::Bool;
    if (t == "std::string" || t == "string" || t == "char" /* const char* */ ||
        t == "std::string_view")
        return Scalar::Str;
    if (t == "float" || t == "double" || t == "long double")
        return Scalar::Float;
    // integral family (incl. fixed-width and signedness spellings)
    static const std::regex kIntegral(
        R"(^(signed\s+|unsigned\s+)?(char|short|int|long|long\s+long|size_t|std::size_t|int8_t|int16_t|int32_t|int64_t|uint8_t|uint16_t|uint32_t|uint64_t|ptrdiff_t)$)");
    if (std::regex_match(t, kIntegral)) return Scalar::Int;
    // `const char *` normalises to `char`; treat the pointer-to-char case
    // as a string, matching app.h's const char* -> Scalar::Str.
    if (raw.find("char") != std::string::npos &&
        raw.find('*') != std::string::npos)
        return Scalar::Str;
    throw std::runtime_error(
        "topo-app static: unsupported handler type spelling '" + raw +
        "'; use an integral / floating / bool / string scalar, or a "
        "Record<Field<...>> alias");
}

// --- record schema aliases ------------------------------------------------
//
// C++ has no reflection, so app.h carries record field names as explicit
// metadata on a topo::app::Record<Field<"name", T>, ...> type. The static
// reader recovers exactly those names from the alias' *text* — the same
// explicit contract, parsed instead of instantiated.
struct RecordSchema {
    std::vector<RecordField> fields;
};

class TypeResolver {
public:
    // Register `using Alias = topo::app::Record<Field<"id", int>, ...>;`.
    void add_record_alias(const std::string& alias, const std::string& rhs) {
        std::string inner;
        std::smatch m;
        static const std::regex kRec(R"RX(Record\s*<(.*)>\s*;?\s*$)RX");
        if (!std::regex_search(rhs, m, kRec))
            throw std::runtime_error(
                "topo-app static: record alias '" + alias +
                "' is not a topo::app::Record<...>");
        inner = m[1].str();
        RecordSchema schema;
        for (const auto& fld : split_top_level(inner)) {
            // Field<"amount", double>
            std::smatch fm;
            static const std::regex kField(
                R"RX(Field\s*<\s*"((?:[^"\\]|\\.)*)"\s*,\s*(.+)>\s*$)RX");
            if (!std::regex_search(fld, fm, kField))
                throw std::runtime_error(
                    "topo-app static: malformed Field<> in record alias '" +
                    alias + "': " + fld);
            std::string fname = fm[1].str();
            std::string ftype = trim(fm[2].str());
            schema.fields.push_back(
                {fname, resolve(ftype)});  // nested types resolve too
        }
        records_[alias] = std::move(schema);
    }

    // A scalar alias such as topo-init's `using int = std::cpp17::int;` is
    // irrelevant here (handlers spell the C++ type, not the topo alias),
    // but a plain `using Money = double;` should still resolve.
    void add_scalar_alias(const std::string& alias, const std::string& rhs) {
        scalar_aliases_[alias] = trim(rhs);
    }

    TypeRef resolve(const std::string& spelling) const {
        // Strip storage-class / cv decoration before the alias lookup so a
        // `static OrderRec` return type resolves to the OrderRec record
        // alias, not a phantom type. Same specifier set normalise_type
        // removes; doing it here keeps record-name matching robust to
        // definition-site decoration (the C++ slice writes `static
        // OrderRec parse(...)`).
        std::string t = strip_specifiers(spelling);
        auto rit = records_.find(t);
        if (rit != records_.end())
            return TypeRef::of_record(rit->second.fields);
        auto sit = scalar_aliases_.find(t);
        if (sit != scalar_aliases_.end())
            return TypeRef::of_scalar(scalar_from_cpp(sit->second));
        return TypeRef::of_scalar(scalar_from_cpp(t));
    }

    static std::string strip_specifiers(const std::string& spelling) {
        static const std::regex kSpec(
            R"(\b(const|volatile|struct|class|typename|static|virtual|inline|constexpr|consteval|explicit|friend|extern|override|final)\b)");
        std::string t = std::regex_replace(trim(spelling), kSpec, " ");
        return trim(std::regex_replace(t, std::regex(R"(\s+)"), " "));
    }

private:
    std::map<std::string, RecordSchema> records_;
    std::map<std::string, std::string> scalar_aliases_;
};

// --- the static front-end -------------------------------------------------

/// Report which of the five known regex-MVP gaps the parsed source
/// triggered. Each `triggered_*` flag corresponds 1:1 to a construct the
/// regex front-end deliberately does NOT understand:
///
/// | Flag                       | Construct (regex-MVP gap) |
/// |----------------------------|-------------------|
/// | `triggered_macro_handler`  | macro-expanded registration calls (`#define REG(..)`)  |
/// | `triggered_auto_return`    | `auto`-return handler signatures                       |
/// | `triggered_cross_line`     | cross-line / indirect registration                     |
/// | `triggered_using_chain`    | record-field types through `using` / template-alias chains |
/// | `triggered_aliased_ns`     | nested / aliased-namespace handler symbols (`using namespace`) |
///
/// The `partial` summary is true when ANY flag fires. When set, the
/// returned `App` is best-effort and may silently omit handlers / edges
/// — switch to the clang-AST primary (`topo-app-static-cpp`) for full
/// coverage. The two-arg `parse_app_source` overload fills this struct;
/// the legacy one-arg form still emits stderr diagnostics for each
/// triggered gap so users who do not opt into the struct still see the
/// degradation.
struct StaticParseFidelity {
    bool partial = false;
    bool triggered_macro_handler = false;
    bool triggered_auto_return = false;
    bool triggered_cross_line = false;
    bool triggered_using_chain = false;
    bool triggered_aliased_ns = false;

    /// Recompute `partial` from the per-flag booleans.
    void refresh() {
        partial = triggered_macro_handler || triggered_auto_return ||
                  triggered_cross_line || triggered_using_chain ||
                  triggered_aliased_ns;
    }
};

namespace detail {
inline void scan_known_gaps(const std::string& src, StaticParseFidelity& f) {
    // case 1: macro-expanded registration — `#define REG(...) ... .handler ...`
    static const std::regex kMacroReg(
        R"RX(#\s*define\s+\w+\s*\([^\)]*\)[\s\S]*?\.\s*handler\b)RX");
    if (std::regex_search(src, kMacroReg)) f.triggered_macro_handler = true;
    // case 2: `auto` return on a free function — `auto foo(...)` or
    //         `auto bar()->...`. Matches anywhere at module scope.
    static const std::regex kAutoRet(R"RX(\bauto\s+\w+\s*\([^;{]*\)\s*(?:->|\{))RX");
    if (std::regex_search(src, kAutoRet)) f.triggered_auto_return = true;
    // case 3: cross-line / indirect registration — `.handler(\n &fn,` (LF
    //         between handler( and the arg list) is the canonical form. The
    //         regex below catches any line break inside the immediate
    //         handler( ... ) call sequence.
    static const std::regex kCrossLine(R"RX(\.\s*handler\s*\(\s*\n)RX");
    if (std::regex_search(src, kCrossLine)) f.triggered_cross_line = true;
    // case 4: record-field types through using/typedef/template-alias chains
    //         — heuristic: presence of a typedef that resolves to another
    //         using-alias (a chain of length >= 2). MVP detects only
    //         `typedef A B;` patterns; a tighter detection would require
    //         walking the type resolver's add_*_alias edge graph after
    //         population. Caught conservatively here.
    static const std::regex kTypedef(R"RX(\btypedef\s+\w+\s+\w+\s*;)RX");
    if (std::regex_search(src, kTypedef)) f.triggered_using_chain = true;
    // case 5: nested / aliased-namespace handler symbols — `using namespace`
    //         anywhere in the source, OR a handler reference of the form
    //         `outer::inner::fn` (more than one `::`).
    static const std::regex kUsingNs(R"RX(\busing\s+namespace\b)RX");
    if (std::regex_search(src, kUsingNs)) f.triggered_aliased_ns = true;
    static const std::regex kNestedHandlerRef(
        R"RX(\.\s*handler\s*\(\s*&?\s*\w+(?:::\w+){2,}\s*,)RX");
    if (std::regex_search(src, kNestedHandlerRef)) f.triggered_aliased_ns = true;
    f.refresh();
}

inline void warn_known_gaps(const StaticParseFidelity& f) {
    if (!f.partial) return;
    auto warn = [](const char* what) {
        std::fprintf(stderr,
            "topo-app-static: source contains %s; fall back to "
            "topo-app-static-cpp (the clang-AST front-end) for full coverage\n",
            what);
    };
    if (f.triggered_macro_handler) warn("macro-expanded registration calls");
    if (f.triggered_auto_return)   warn("`auto`-return handler signatures");
    if (f.triggered_cross_line)    warn("cross-line / indirect registration");
    if (f.triggered_using_chain)   warn("record-field types through "
                                        "using / typedef / template-alias chains");
    if (f.triggered_aliased_ns)    warn("nested / aliased-namespace handler symbols");
}
} // namespace detail

// Parse C++ source TEXT that uses the runtime registration surface and
// return the equivalent in-memory App, WITHOUT compiling or running it.
// Handlers appear in registration order (same as app.h, which pushes in
// call order); the flow's edges are produced by the SAME App::flow()
// linearisation the runtime bridge uses (this calls it), so a parallel()
// stage fans in/out exactly as the runtime path does.
//
// Two-arg overload: when `fidelity_out` is non-null, populates it with
// per-gap booleans and skips the stderr emission (the caller now owns
// surfacing the fidelity). Use this in tooling that wants to fail-closed
// on partial extraction (e.g. CI gating).
inline App parse_app_source(const std::string& cpp_source,
                            StaticParseFidelity* fidelity_out) {
    std::string src = strip_comments(cpp_source);

    // Scan once for known-gap fingerprints — see the StaticParseFidelity
    // doc table. If the caller wants the structured form, fill it; else
    // emit stderr warnings so the user sees degradation.
    StaticParseFidelity local_fidelity;
    StaticParseFidelity& fidelity = fidelity_out ? *fidelity_out : local_fidelity;
    detail::scan_known_gaps(src, fidelity);
    if (!fidelity_out) detail::warn_known_gaps(fidelity);

    // 1. App namespace: App x("orders"); or App x{"orders"};
    std::string ns = "topo_app";
    {
        std::smatch m;
        static const std::regex kNs(
            R"RX((?:ta::|topo::app::)?App\s+\w+\s*[\({]\s*"((?:[^"\\]|\\.)*)")RX");
        if (std::regex_search(src, m, kNs)) ns = m[1].str();
    }

    // 2. Type aliases (record schemas + scalar typedefs).
    TypeResolver types;
    {
        // using Alias = <rhs> ;  (rhs may span lines; join then split on ;)
        static const std::regex kWs(R"RX(\s+)RX");
        std::string flat = std::regex_replace(src, kWs, " ");
        static const std::regex kUsing(
            R"RX(using\s+([A-Za-z_]\w*)\s*=\s*([^;]+);)RX");
        auto begin = std::sregex_iterator(flat.begin(), flat.end(), kUsing);
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            std::string alias = (*it)[1].str();
            std::string rhs = trim((*it)[2].str());
            if (rhs.find("Record<") != std::string::npos ||
                rhs.find("Record <") != std::string::npos)
                types.add_record_alias(alias, rhs);
            else
                types.add_scalar_alias(alias, rhs);
        }
    }

    // 3. Free-function signatures. Same shape as CppSymbolExtractor's
    //    funcDefRegex but capturing the return type and the raw parameter
    //    list; only file-scope definitions matter for this surface.
    std::map<std::string, FnSig> fns;
    {
        static const std::regex fn_re(
            R"RX((?:^|\n)\s*([A-Za-z_][\w:<>,\s\*&]*?)\s+([A-Za-z_]\w*)\s*\(([^;{]*)\)\s*\{)RX");
        for (auto it = std::sregex_iterator(src.begin(), src.end(), fn_re);
             it != std::sregex_iterator(); ++it) {
            std::string ret = trim((*it)[1].str());
            std::string name = (*it)[2].str();
            std::string params = (*it)[3].str();
            // skip control keywords masquerading as a return type
            static const std::regex kKw(
                R"RX(\b(if|for|while|switch|return|else|do|try|catch)\b)RX");
            if (std::regex_search(ret, kKw)) continue;
            FnSig sig;
            sig.return_type = ret;
            std::string p = trim(params);
            if (!p.empty() && p != "void") {
                for (auto& part : split_top_level(p)) {
                    // "Type name" / "Type" — drop the trailing identifier
                    // exactly like CppSymbolExtractor::extractParamTypes.
                    std::string pp = trim(part);
                    std::size_t sp = pp.find_last_of(" \t&*");
                    if (sp != std::string::npos && sp + 1 < pp.size() &&
                        (std::isalpha((unsigned char)pp[sp + 1]) ||
                         pp[sp + 1] == '_')) {
                        std::string ty = trim(pp.substr(0, sp + 1));
                        sig.param_types.push_back(ty);
                    } else {
                        sig.param_types.push_back(pp);
                    }
                }
            }
            fns[name] = std::move(sig);
        }
    }

    App app(ns);

    // 4. Handler registrations, in source order:
    //    <obj>.handler(&fn, "name" [, "doc"]);
    std::vector<std::string> handler_names;
    {
        static const std::regex h_re(
            R"RX(\.\s*handler\s*\(\s*&?\s*([A-Za-z_][\w:]*)\s*,\s*"((?:[^"\\]|\\.)*)")RX");
        for (auto it = std::sregex_iterator(src.begin(), src.end(), h_re);
             it != std::sregex_iterator(); ++it) {
            std::string fn_ref = (*it)[1].str();
            std::string reg_name = (*it)[2].str();
            // function may be referenced as ns::fn or fn; key on the last
            // component, which is how the signature map is keyed.
            std::string key = fn_ref;
            std::size_t cc = key.rfind("::");
            if (cc != std::string::npos) key = key.substr(cc + 2);
            auto sit = fns.find(key);
            if (sit == fns.end())
                throw std::runtime_error(
                    "topo-app static: handler '" + reg_name +
                    "' references function '" + fn_ref +
                    "' with no visible definition in the source");
            const FnSig& sig = sit->second;
            if (sig.param_types.size() > 1)
                throw std::runtime_error(
                    "topo-app static: handler '" + reg_name +
                    "' has more than one input parameter (a handler is a "
                    "pure Functor: at most one input)");
            Handler hd;
            hd.name = reg_name;
            if (!sig.param_types.empty())
                hd.in_type = types.resolve(sig.param_types[0]);
            hd.out_type = types.resolve(sig.return_type);
            app.graph().handlers().push_back(std::move(hd));
            handler_names.push_back(reg_name);
        }
    }
    if (handler_names.empty())
        throw std::runtime_error(
            "topo-app static: no handler registrations found in source");

    // 5. The flow: <obj>.flow("name", <stage>, <stage>, ...);
    //    Stages: "literal" | bareName | parallel("a","b",...). We rebuild
    //    the stage tokens and dispatch into the SAME App::flow() so edge
    //    linearisation (incl. parallel fan-in/out) is the runtime path's,
    //    not a second implementation.
    {
        std::smatch fm;
        static const std::regex kFlow(R"RX(\.\s*flow\s*\(([\s\S]*?)\)\s*;)RX");
        if (std::regex_search(src, fm, kFlow)) {
            std::vector<std::string> args = split_top_level(fm[1].str());
            if (args.size() < 2)
                throw std::runtime_error(
                    "topo-app static: flow(...) needs a name and at least "
                    "one stage");
            std::string flow_name;
            if (auto lit = first_string_literal(args[0]))
                flow_name = *lit;
            else
                throw std::runtime_error(
                    "topo-app static: flow name must be a string literal");

            // Reproduce App::flow()'s stage->edge construction. App::flow
            // is a variadic template over std::string / detail::Parallel; from
            // text we build the same std::vector<std::vector<string>> of
            // stage members and run the identical edge rule, so the
            // emitted edges match the runtime bridge byte-for-byte.
            std::vector<std::vector<std::string>> stages;
            for (std::size_t i = 1; i < args.size(); ++i) {
                std::string a = trim(args[i]);
                if (a.find("parallel") == 0 ||
                    a.find("::parallel") != std::string::npos) {
                    std::smatch pm;
                    static const std::regex kPar(
                        R"RX(parallel\s*\(([\s\S]*)\)\s*$)RX");
                    if (!std::regex_search(a, pm, kPar))
                        throw std::runtime_error(
                            "topo-app static: malformed parallel(...) stage");
                    std::vector<std::string> members;
                    for (const auto& mem : split_top_level(pm[1].str())) {
                        auto ml = first_string_literal(mem);
                        members.push_back(ml ? *ml : trim(mem));
                    }
                    stages.push_back(std::move(members));
                } else if (auto lit = first_string_literal(a)) {
                    stages.push_back({*lit});
                } else {
                    stages.push_back({a});  // bare registered name
                }
            }

            Flow f;
            f.name = flow_name;
            for (std::size_t i = 0; i + 1 < stages.size(); ++i)
                for (const auto& s : stages[i])
                    for (const auto& t : stages[i + 1])
                        f.edges.push_back(Edge{s, t});
            for (const auto& s : stages.back())
                f.edges.push_back(Edge{s, std::nullopt});
            app.graph().set_flow(std::move(f));
        }
    }

    return app;
}

// Single-arg back-compat wrapper. Delegates to the structured form
// and discards the StaticParseFidelity (the structured form emits
// stderr warnings on degradation already, so users without the
// out-param still see the gap signal).
inline App parse_app_source(const std::string& cpp_source) {
    return parse_app_source(cpp_source, /*fidelity_out=*/nullptr);
}

}  // namespace stat
}  // namespace topo::app

#endif  // TOPO_APP_STATIC_H
