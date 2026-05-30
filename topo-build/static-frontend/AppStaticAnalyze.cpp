#include "AppStaticAnalyze.h"

#include "topo/Platform/Process.h"
#include "topo/Platform/TempFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

// topo-app C++ static front-end — clang-AST analyze stage.
//
// Walks `clang -Xclang -ast-dump=json` output and reconstructs the same
// topo::app::Graph the runtime registration bridge (app.h) builds. See
// AppStaticAnalyze.h for the AST-acquisition decision and the boundary.
//
// AST-shape notes (clang -ast-dump=json, stable since LLVM 9; bundled
// LLVM is 22.x):
//   * FunctionDecl: { kind, name, type:{qualType:"Ret (P0, P1)"},
//                     inner:[ ParmVarDecl{type:{qualType}}, ... ,
//                             CompoundStmt | ... ] }.
//     For an `auto`-return handler clang reports the *deduced* return
//     type in qualType — limitation 2 dissolves with no special case.
//   * TypedefDecl / TypeAliasDecl: { kind, name, type:{qualType} } —
//     the aliased type as written; chains resolve by repeated lookup,
//     and clang's canonical spelling desugars nested aliases —
//     limitation 4.
//   * NamespaceDecl / NamespaceAliasDecl: walked recursively, so a
//     handler defined in a nested or aliased namespace is still found —
//     limitation 5.
//   * CXXMemberCallExpr for `obj.handler(...)` / `obj.flow(...)`: the
//     call exists in the AST regardless of source-line layout or
//     macro expansion — limitations 1 and 3.

namespace topo::app::staticfe {

namespace {

using json = nlohmann::json;

// --- small text helpers (shared shape with app_static.h's regex MVP) ------

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Collapse internal whitespace runs to a single space.
std::string collapseWs(const std::string& s) {
    std::string out;
    bool inWs = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            inWs = true;
        } else {
            if (inWs && !out.empty()) out += ' ';
            inWs = false;
            out += c;
        }
    }
    return out;
}

// Split a fragment on top-level commas — angle / paren / brace / bracket
// and string-literal aware. The same depth-tracked split app_static.h
// uses; here it splits the *clang-reported* template-argument spelling
// of a Record<...> / Field<...> type, not raw user source.
std::vector<std::string> splitTopLevel(const std::string& s, char sep = ',') {
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
        if (c == '<' || c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == '>' || c == ')' || c == ']' || c == '}')
            --depth;
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

// Strip cv / storage-class / specifier keywords and pointer/reference
// punctuation, leaving the bare type spelling. Same keyword set
// app_static.h::normalise_type strips, applied to a clang qualType.
std::string normaliseType(std::string t) {
    t = collapseWs(trim(t));
    static const char* kKeywords[] = {
        "const ",  "volatile ", "struct ",   "class ",   "typename ",
        "static ", "virtual ",  "inline ",   "constexpr ", "consteval ",
        "explicit ", "friend ", "extern ",   "override ", "final "};
    for (const char* kw : kKeywords) {
        std::string k = kw;
        std::size_t pos;
        while ((pos = t.find(k)) != std::string::npos)
            t.erase(pos, k.size());
    }
    // also a trailing `const` (east-const)
    while (t.size() >= 6 && t.compare(t.size() - 6, 6, " const") == 0)
        t.erase(t.size() - 6);
    std::string o;
    for (char c : t)
        if (c != '&' && c != '*') o += c;
    return collapseWs(trim(o));
}

// Drop a leading `topo::app::` / `ta::` namespace qualifier from a
// recognised surface type name so `ta::Record<...>` and `Record<...>`
// compare equal.
std::string stripSurfaceQualifier(const std::string& s) {
    static const char* kPrefixes[] = {"topo::app::", "ta::"};
    std::string t = s;
    for (const char* p : kPrefixes) {
        std::string pre = p;
        while (t.compare(0, pre.size(), pre) == 0) t.erase(0, pre.size());
    }
    return t;
}

// --- C++ type spelling -> topo::app scalar band ---------------------------
//
// The static analogue of app.h's scalar_of<>. The four stdlib bands and
// the bool-before-integral precedence match app.h exactly so a
// statically built Graph is indistinguishable from a runtime-built one.

std::optional<Scalar> scalarOf(const std::string& raw) {
    std::string t = normaliseType(raw);
    // drop a `std::` qualifier for the comparison
    if (t == "bool") return Scalar::Bool;
    if (t == "std::string" || t == "string" ||
        t == "std::__cxx11::basic_string<char>" ||
        t == "std::basic_string<char>" || t == "std::string_view" ||
        t == "std::basic_string_view<char>")
        return Scalar::Str;
    if (t == "char" || t == "const char")  // pointer already stripped
        return Scalar::Str;  // const char* -> str (matches app.h)
    if (t == "float" || t == "double" || t == "long double")
        return Scalar::Float;
    static const char* kIntegral[] = {
        "char",        "signed char",  "unsigned char", "short",
        "unsigned short", "int",       "unsigned int",  "unsigned",
        "long",        "unsigned long", "long long",    "unsigned long long",
        "size_t",      "std::size_t",  "ptrdiff_t",     "ssize_t",
        "int8_t",      "int16_t",      "int32_t",       "int64_t",
        "uint8_t",     "uint16_t",     "uint32_t",      "uint64_t",
        "std::int8_t", "std::int16_t", "std::int32_t",  "std::int64_t",
        "std::uint8_t","std::uint16_t","std::uint32_t", "std::uint64_t"};
    for (const char* k : kIntegral)
        if (t == k) return Scalar::Int;
    return std::nullopt;
}

// --- the analysis context -------------------------------------------------

// A registered handler reference, recovered from a `.handler(...)` call.
//
// `fnId` is the clang decl id of the referenced function — a unique
// per-decl handle that resolves the EXACT function even when two
// functions in different namespaces share a bare name (limitation 5).
// `fnName` is the bare name, kept for diagnostics and as a fallback when
// the AST gave no referencedDecl id.
struct Registration {
    std::string fnId;
    std::string fnName;
    std::string regName;  // the registered handler name
};

// A flow stage: one or more parallel member names.
using Stage = std::vector<std::string>;

class Analyzer {
public:
    explicit Analyzer(Result& result) : r_(result) {}

    // Walk the whole translation unit.
    void run(const json& tu) {
        // Pass 1: collect type aliases, record/struct definitions and
        // function signatures from every namespace depth.
        collectDecls(tu);
        // Pass 2: find the App namespace, the handler registrations and
        // the flow from the call expressions.
        collectCalls(tu);
        build();
    }

private:
    Result& r_;

    // typedef / using : alias name -> aliased type spelling.
    std::map<std::string, std::string> aliases_;
    // record-like struct/class : name -> ordered (field, type) list.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>>
        structs_;
    // free / member function signature.
    struct FnSig {
        std::string name;  // bare name (for diagnostics)
        std::string returnType;
        std::vector<std::string> paramTypes;
        bool defined = false;  // a body was present
    };
    // Keyed by the clang decl id — unique per function, so two functions
    // in different namespaces that share a bare name never collide
    // (the flat-keying weakness of the regex MVP — limitation 5).
    std::map<std::string, FnSig> fnsById_;
    // Bare name -> the decl ids defining it; used only as a fallback
    // when a registration's referencedDecl carried no id.
    std::map<std::string, std::vector<std::string>> fnsByName_;

    std::optional<std::string> namespace_;
    std::vector<Registration> registrations_;
    std::optional<std::pair<std::string, std::vector<Stage>>> flow_;

    void note(const std::string& msg) {
        r_.unsupported.push_back(msg);
        r_.fidelity = Fidelity::Degraded;
    }

    static std::string nodeKind(const json& n) {
        auto it = n.find("kind");
        return it != n.end() && it->is_string() ? it->get<std::string>()
                                                : std::string{};
    }
    static std::string nodeName(const json& n) {
        auto it = n.find("name");
        return it != n.end() && it->is_string() ? it->get<std::string>()
                                                : std::string{};
    }
    static std::string qualType(const json& n) {
        auto t = n.find("type");
        if (t == n.end() || !t->is_object()) return {};
        auto q = t->find("qualType");
        return q != t->end() && q->is_string() ? q->get<std::string>()
                                               : std::string{};
    }
    // The fully-desugared type spelling: clang emits `desugaredQualType`
    // on a `type` object when the written form carries sugar (a typedef,
    // a `using` alias, a template alias). Using it collapses an entire
    // `using`/`typedef`/template-alias chain in one step (limitation 4)
    // — `desugaredQualType` strips every layer of sugar at once. Falls
    // back to the sugared `qualType` when no sugar is present.
    static std::string desugaredType(const json& n) {
        auto t = n.find("type");
        if (t == n.end() || !t->is_object()) return {};
        auto d = t->find("desugaredQualType");
        if (d != t->end() && d->is_string() && !d->get<std::string>().empty())
            return d->get<std::string>();
        auto q = t->find("qualType");
        return q != t->end() && q->is_string() ? q->get<std::string>()
                                               : std::string{};
    }
    // The clang decl id — a unique per-node hex handle. A DeclRefExpr's
    // referencedDecl carries the id of exactly the decl the name bound
    // to; matching on it is namespace-overload-exact.
    static std::string nodeId(const json& n) {
        auto it = n.find("id");
        return it != n.end() && it->is_string() ? it->get<std::string>()
                                                : std::string{};
    }

    // --- pass 1: declarations ---------------------------------------------

    // Walk every declaration in the translation unit, descending through
    // every NamespaceDecl so a nested / aliased-namespace handler is
    // still discovered (limitation 5). A FunctionDecl's body is not
    // recursed into — only its signature matters for pass 1.
    void collectDecls(const json& node) {
        std::string k = nodeKind(node);

        if (k == "TypedefDecl" || k == "TypeAliasDecl") {
            std::string name = nodeName(node);
            // The desugared RHS collapses every nested alias / typedef /
            // template-alias layer in one step, so a chained record-field
            // type resolves directly (limitation 4).
            std::string rhs = desugaredType(node);
            if (!name.empty() && !rhs.empty())
                aliases_[name] = rhs;
            return;  // a typedef has no decl children of interest
        }
        if (k == "FunctionDecl" || k == "CXXMethodDecl") {
            collectFunction(node);
            return;  // do not recurse into the body
        }
        if (k == "CXXRecordDecl" ||
            k == "ClassTemplateSpecializationDecl") {
            // a plain struct/class used as a handler In/Out aggregate.
            std::string name = nodeName(node);
            bool isComplete = node.contains("completeDefinition") &&
                              node["completeDefinition"].is_boolean() &&
                              node["completeDefinition"].get<bool>();
            if (!name.empty() && isComplete) {
                std::vector<std::pair<std::string, std::string>> fields;
                if (auto inner = node.find("inner");
                    inner != node.end() && inner->is_array()) {
                    for (const auto& c : *inner) {
                        if (nodeKind(c) == "FieldDecl") {
                            std::string fn = nodeName(c);
                            // desugared so an aliased field type resolves.
                            std::string ft = desugaredType(c);
                            if (!fn.empty() && !ft.empty())
                                fields.emplace_back(fn, ft);
                        }
                    }
                }
                if (!fields.empty()) structs_[name] = std::move(fields);
            }
            // a record may still nest typedefs / member structs — recurse.
        }

        // recurse into TU / namespace / record children.
        if (auto inner = node.find("inner");
            inner != node.end() && inner->is_array())
            for (const auto& c : *inner) collectDecls(c);
    }

    void collectFunction(const json& node) {
        std::string name = nodeName(node);
        std::string id = nodeId(node);
        if (name.empty() || id.empty()) return;
        // The App's own member functions (handler/flow) are surface
        // machinery, never user handlers — skip them.
        if (name == "handler" || name == "flow") return;

        FnSig sig;
        sig.name = name;
        // return type: the function type is "Ret (P0, P1)"; everything
        // before the last top-level '(' is the return type. The
        // desugared form is used so an aliased return type — and an
        // `auto` return, which clang has already deduced — resolves
        // directly (limitations 2 and 4).
        std::string qt = desugaredType(node);
        sig.returnType = returnTypeFromQual(qt);

        bool bodySeen = false;
        if (auto inner = node.find("inner");
            inner != node.end() && inner->is_array()) {
            for (const auto& c : *inner) {
                std::string ck = nodeKind(c);
                if (ck == "ParmVarDecl") {
                    sig.paramTypes.push_back(desugaredType(c));
                } else if (ck == "CompoundStmt" || ck.rfind("Stmt") !=
                                                       std::string::npos ||
                           ck.rfind("Expr") != std::string::npos) {
                    bodySeen = true;
                }
            }
        }
        sig.defined = bodySeen;
        fnsById_[id] = std::move(sig);
        auto& ids = fnsByName_[name];
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    }

    // "Ret (P0, P1)" -> "Ret"; depth-aware so a function-pointer-returning
    // signature does not confuse the split.
    static std::string returnTypeFromQual(const std::string& qt) {
        int depth = 0;
        std::size_t parenAt = std::string::npos;
        for (std::size_t i = 0; i < qt.size(); ++i) {
            char c = qt[i];
            if (c == '<' || c == '[') ++depth;
            else if (c == '>' || c == ']') --depth;
            else if (c == '(' && depth == 0) {
                parenAt = i;
                break;
            }
        }
        return parenAt == std::string::npos ? trim(qt)
                                            : trim(qt.substr(0, parenAt));
    }

    // --- pass 2: registration / flow calls --------------------------------

    void collectCalls(const json& node) {
        std::string k = nodeKind(node);

        if (k == "CXXConstructExpr" || k == "VarDecl") {
            // `App app("orders");` / `App app{"orders"}` — capture the
            // namespace string from a construction whose declared type is
            // the topo-app App.
            std::string ty = stripSurfaceQualifier(normaliseType(qualType(node)));
            if (ty == "App") {
                std::string ns = firstStringLiteral(node);
                if (!ns.empty() && !namespace_.has_value()) namespace_ = ns;
            }
        }
        if (k == "CXXMemberCallExpr" || k == "CallExpr") {
            handleCall(node);
        }

        if (auto inner = node.find("inner");
            inner != node.end() && inner->is_array())
            for (const auto& c : *inner) collectCalls(c);
    }

    // Recover the member name of a `obj.member(...)` call: the call's
    // first inner child is a MemberExpr carrying `name`.
    static std::string memberCallName(const json& call) {
        auto inner = call.find("inner");
        if (inner == call.end() || !inner->is_array() || inner->empty())
            return {};
        const json& callee = (*inner)[0];
        if (nodeKind(callee) == "MemberExpr") return nodeName(callee);
        // Some forms wrap the MemberExpr in an ImplicitCastExpr.
        if (auto ci = callee.find("inner");
            ci != callee.end() && ci->is_array() && !ci->empty()) {
            const json& m = (*ci)[0];
            if (nodeKind(m) == "MemberExpr") return nodeName(m);
        }
        return {};
    }

    void handleCall(const json& call) {
        std::string member = memberCallName(call);
        if (member.empty()) {
            // a free CallExpr (e.g. App::App via a factory) — also check
            // for `App::App` style namespace capture is done elsewhere.
            return;
        }
        auto inner = call.find("inner");
        if (inner == call.end() || !inner->is_array()) return;

        // arguments are every inner child after the callee (index 0).
        std::vector<const json*> args;
        for (std::size_t i = 1; i < inner->size(); ++i)
            args.push_back(&(*inner)[i]);

        if (member == "handler") {
            // obj.handler(&fn, "name" [, "doc"]).
            json empty;
            std::string fnId =
                declRefId(args.empty() ? empty : *args[0]);
            std::string fnName =
                declRefName(args.empty() ? empty : *args[0]);
            std::string regName;
            if (args.size() >= 2) regName = stringLiteralValue(*args[1]);
            if (fnId.empty() && fnName.empty()) {
                note("handler registration whose function argument is not a "
                     "direct function reference (indirect / computed callable "
                     "is outside the topo-app surface)");
                return;
            }
            if (regName.empty()) regName = fnName;
            registrations_.push_back({fnId, fnName, regName});
        } else if (member == "flow") {
            // obj.flow("name", <stage>, <stage>, ...).
            if (args.empty()) return;
            std::string flowName = stringLiteralValue(*args[0]);
            if (flowName.empty()) {
                note("flow(...) whose name is not a string literal");
                return;
            }
            std::vector<Stage> stages;
            for (std::size_t i = 1; i < args.size(); ++i) {
                Stage st = stageFromArg(*args[i]);
                if (!st.empty()) stages.push_back(std::move(st));
            }
            if (!stages.empty())
                flow_ = std::make_pair(flowName, std::move(stages));
        }
    }

    // A flow stage argument: a string literal "name", or a
    // `parallel("a","b",...)` call.
    Stage stageFromArg(const json& arg) {
        // unwrap implicit casts / material-temporary / construct-expr.
        const json* cur = &arg;
        for (int guard = 0; guard < 8; ++guard) {
            std::string k = nodeKind(*cur);
            if (k == "StringLiteral") {
                return {stringLiteralValue(*cur)};
            }
            if (k == "CallExpr") {
                // parallel(...) — its callee names `parallel`.
                std::string fn = calleeName(*cur);
                if (stripSurfaceQualifier(fn) == "parallel") {
                    Stage members;
                    if (auto inner = cur->find("inner");
                        inner != cur->end() && inner->is_array()) {
                        for (std::size_t i = 1; i < inner->size(); ++i) {
                            std::string m =
                                stringLiteralValue((*inner)[i]);
                            if (!m.empty()) members.push_back(m);
                        }
                    }
                    return members;
                }
            }
            // descend through the first inner child.
            auto inner = cur->find("inner");
            if (inner == cur->end() || !inner->is_array() || inner->empty())
                break;
            cur = &(*inner)[0];
        }
        // a bare string literal somewhere inside.
        std::string lit = stringLiteralValue(arg);
        if (!lit.empty()) return {lit};
        note("flow stage argument outside the topo-app surface (expected a "
             "string-literal handler name or parallel(...))");
        return {};
    }

    // The callee name of a CallExpr (a DeclRefExpr to a function).
    static std::string calleeName(const json& call) {
        auto inner = call.find("inner");
        if (inner == call.end() || !inner->is_array() || inner->empty())
            return {};
        return declRefName((*inner)[0]);
    }

    // Recover the referenced name from a DeclRefExpr / UnaryOperator
    // (`&fn`) / ImplicitCastExpr chain. Returns the last `::` component.
    static std::string declRefName(const json& node) {
        if (node.is_null()) return {};
        std::string k = nodeKind(node);
        if (k == "DeclRefExpr") {
            // referencedDecl carries the function's name; namespace
            // nesting is irrelevant — keep the last component.
            auto rd = node.find("referencedDecl");
            if (rd != node.end() && rd->is_object()) {
                std::string nm = nodeName(*rd);
                if (!nm.empty()) return lastComponent(nm);
            }
            std::string nm = nodeName(node);
            if (!nm.empty()) return lastComponent(nm);
        }
        if (auto inner = node.find("inner");
            inner != node.end() && inner->is_array())
            for (const auto& c : *inner) {
                std::string got = declRefName(c);
                if (!got.empty()) return got;
            }
        return {};
    }

    // The clang decl id of the function a `&fn` / `fn` argument refers
    // to, dug out of the DeclRefExpr's referencedDecl. This is the
    // namespace-exact handle that resolves an overloaded / namespace-
    // collided bare name to the precise function (limitation 5).
    static std::string declRefId(const json& node) {
        if (node.is_null()) return {};
        if (nodeKind(node) == "DeclRefExpr") {
            auto rd = node.find("referencedDecl");
            if (rd != node.end() && rd->is_object()) {
                std::string id = nodeId(*rd);
                if (!id.empty()) return id;
            }
        }
        if (auto inner = node.find("inner");
            inner != node.end() && inner->is_array())
            for (const auto& c : *inner) {
                std::string got = declRefId(c);
                if (!got.empty()) return got;
            }
        return {};
    }

    static std::string lastComponent(const std::string& s) {
        std::size_t cc = s.rfind("::");
        return cc == std::string::npos ? s : s.substr(cc + 2);
    }

    // The value of a StringLiteral node (clang reports it WITH quotes in
    // the `value` field, e.g. "\"orders\"").
    static std::string stringLiteralValue(const json& node) {
        std::string k = nodeKind(node);
        if (k == "StringLiteral") {
            auto v = node.find("value");
            if (v != node.end() && v->is_string()) {
                std::string s = v->get<std::string>();
                return unquote(s);
            }
        }
        if (auto inner = node.find("inner");
            inner != node.end() && inner->is_array())
            for (const auto& c : *inner) {
                std::string got = stringLiteralValue(c);
                if (!got.empty()) return got;
            }
        return {};
    }

    // The first string literal anywhere under a node.
    static std::string firstStringLiteral(const json& node) {
        return stringLiteralValue(node);
    }

    // Strip surrounding double quotes and decode the simple escapes
    // clang emits in a StringLiteral `value`.
    static std::string unquote(const std::string& raw) {
        std::string s = raw;
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[i + 1];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    default: out += n; break;
                }
                ++i;
            } else {
                out += s[i];
            }
        }
        return out;
    }

    // --- type resolution: spelling -> topo::app::TypeRef ------------------

    // Resolve a C++ type spelling (a clang qualType) to a TypeRef.
    // Resolves through `using`/`typedef` chains and `Record<Field<...>>`
    // / plain-struct aggregates. `depth` guards against an alias cycle.
    std::optional<TypeRef> resolveType(const std::string& spelling,
                                       int depth = 0) {
        if (depth > 16) return std::nullopt;
        std::string t = normaliseType(spelling);
        std::string bare = stripSurfaceQualifier(t);

        // 1. a topo-app Record<Field<...>, ...> aggregate (possibly the
        //    RHS of a `using` alias). The leading token must be exactly
        //    `Record` followed by `<` — not a user type merely starting
        //    with "Record".
        {
            std::size_t lt = bare.find('<');
            if (lt != std::string::npos &&
                trim(bare.substr(0, lt)) == "Record")
                return resolveRecordTemplate(bare, depth);
        }

        // 2. a `using`/`typedef` alias — follow the chain. The aliased
        //    spelling clang reports already desugars nested aliases for
        //    a canonical type, but a sugared alias name is followed here.
        if (auto it = aliases_.find(bare); it != aliases_.end())
            return resolveType(it->second, depth + 1);
        if (auto it = aliases_.find(t); it != aliases_.end())
            return resolveType(it->second, depth + 1);

        // 3. a plain struct/class with collected fields.
        if (auto it = structs_.find(bare); it != structs_.end())
            return resolveStruct(it->second, depth);
        if (auto it = structs_.find(t); it != structs_.end())
            return resolveStruct(it->second, depth);

        // 4. a stdlib scalar band.
        if (auto sc = scalarOf(t)) return TypeRef::of_scalar(*sc);

        return std::nullopt;
    }

    std::optional<TypeRef> resolveStruct(
        const std::vector<std::pair<std::string, std::string>>& fields,
        int depth) {
        std::vector<RecordField> out;
        for (const auto& [fn, ft] : fields) {
            auto ref = resolveType(ft, depth + 1);
            if (!ref) return std::nullopt;
            out.emplace_back(fn, *ref);
        }
        return TypeRef::of_record(std::move(out));
    }

    // `Record<Field<"id", int>, Field<"amount", Money>>` -> TypeRef.
    // The `Field<"name", T>` argument carries the field name as an NTTP
    // string literal; the type `T` resolves recursively (through aliases
    // too — limitation 4).
    std::optional<TypeRef> resolveRecordTemplate(const std::string& spelling,
                                                 int depth) {
        std::size_t lt = spelling.find('<');
        std::size_t gt = spelling.rfind('>');
        if (lt == std::string::npos || gt == std::string::npos || gt <= lt)
            return std::nullopt;
        std::string inner = spelling.substr(lt + 1, gt - lt - 1);
        std::vector<RecordField> fields;
        for (const auto& fld : splitTopLevel(inner)) {
            std::string f = stripSurfaceQualifier(trim(fld));
            // Field<"name", Type> — the leading token must be exactly
            // `Field` followed by `<`.
            std::size_t flt = f.find('<');
            std::size_t fgt = f.rfind('>');
            if (flt == std::string::npos || fgt == std::string::npos ||
                trim(f.substr(0, flt)) != "Field") {
                // not a Field<> — clang rendered something the surface
                // does not cover; reject rather than guess.
                note("record alias field '" + fld +
                     "' is not a topo::app::Field<\"name\", T> — clang-AST "
                     "front-end needs the explicit Field<> form");
                return std::nullopt;
            }
            std::string fargs = f.substr(flt + 1, fgt - flt - 1);
            std::vector<std::string> parts = splitTopLevel(fargs);
            if (parts.size() != 2) return std::nullopt;
            std::string fname = stripFieldNameLiteral(parts[0]);
            if (fname.empty()) {
                note("record field name '" + parts[0] +
                     "' is not a recoverable string-literal NTTP");
                return std::nullopt;
            }
            auto ftype = resolveType(parts[1], depth + 1);
            if (!ftype) return std::nullopt;
            fields.emplace_back(fname, *ftype);
        }
        if (fields.empty()) return std::nullopt;
        return TypeRef::of_record(std::move(fields));
    }

    // A Field<>'s first template argument is the field name, carried as
    // a class-type NTTP (the app.h `fixed_string`). clang's TypePrinter
    // may render that NTTP several ways depending on version:
    //
    //   * a quoted literal               Field<"id", int>
    //   * an aggregate around a literal  Field<{"id"}, int>
    //   * a named-type aggregate         Field<fixed_string<3>{"id"}, int>
    //   * a decimal char-code aggregate  Field<{{105, 100, 0}}, int>
    //
    // Recover the name from any of them: prefer an inner quoted string,
    // fall back to decoding a decimal char-code array. Returns empty if
    // neither shape is present (the caller then records it as an
    // unsupported construct rather than guessing).
    static std::string stripFieldNameLiteral(const std::string& raw) {
        std::string s = trim(raw);
        // 1. quoted-literal form.
        std::size_t q1 = s.find('"');
        if (q1 != std::string::npos) {
            std::size_t q2 = s.find('"', q1 + 1);
            if (q2 != std::string::npos)
                return s.substr(q1 + 1, q2 - q1 - 1);
        }
        // 2. decimal char-code array form: pull every integer, treat it
        //    as a NUL-terminated char sequence.
        std::string decoded;
        std::string num;
        bool sawDigit = false;
        for (char c : s) {
            if (c >= '0' && c <= '9') {
                num += c;
                sawDigit = true;
            } else if (!num.empty()) {
                int v = 0;
                try {
                    v = std::stoi(num);
                } catch (...) {
                    v = 0;
                }
                num.clear();
                if (v == 0) break;  // NUL terminator
                if (v > 0 && v < 128) decoded += static_cast<char>(v);
            }
        }
        if (!num.empty()) {
            int v = 0;
            try {
                v = std::stoi(num);
            } catch (...) {
                v = 0;
            }
            if (v > 0 && v < 128) decoded += static_cast<char>(v);
        }
        if (sawDigit && !decoded.empty()) return decoded;
        return {};
    }

    // Resolve a registration to the exact function signature. The decl
    // id is tried first (namespace-overload-exact); if a registration
    // carried no id (older AST shapes) or the id matched a body-less
    // forward declaration, fall back to the bare name — preferring a
    // defined sig so a `parse` with a real body wins over a prototype.
    const FnSig* resolveFunction(const Registration& reg) {
        if (!reg.fnId.empty()) {
            auto it = fnsById_.find(reg.fnId);
            if (it != fnsById_.end() && it->second.defined)
                return &it->second;
            // id matched only a prototype — promote to a defined sig of
            // the same bare name if one exists.
            if (it != fnsById_.end()) {
                if (auto* d = definedSigByName(it->second.name)) return d;
                return &it->second;  // genuinely a prototype-only fn
            }
        }
        if (!reg.fnName.empty()) {
            if (auto* d = definedSigByName(reg.fnName)) return d;
            auto ni = fnsByName_.find(reg.fnName);
            if (ni != fnsByName_.end() && !ni->second.empty())
                return &fnsById_.at(ni->second.front());
        }
        return nullptr;
    }

    // The defined (body-bearing) signature for a bare name, if exactly
    // such a definition exists.
    const FnSig* definedSigByName(const std::string& name) {
        auto ni = fnsByName_.find(name);
        if (ni == fnsByName_.end()) return nullptr;
        for (const auto& id : ni->second) {
            auto si = fnsById_.find(id);
            if (si != fnsById_.end() && si->second.defined)
                return &si->second;
        }
        return nullptr;
    }

    // --- build the Graph --------------------------------------------------

    void build() {
        if (!namespace_.has_value()) {
            r_.ok = false;
            r_.error =
                "no topo-app App(\"<namespace>\") construction found — the "
                "static front-end needs a statically visible App namespace";
            return;
        }

        Graph g(*namespace_);

        for (const auto& reg : registrations_) {
            const FnSig* sigPtr = resolveFunction(reg);
            if (!sigPtr) {
                r_.ok = false;
                r_.error = "registered handler '" + reg.regName +
                           "' references function '" +
                           (reg.fnName.empty() ? reg.fnId : reg.fnName) +
                           "' with no visible definition in the analyzed "
                           "translation unit";
                return;
            }
            const FnSig& sig = *sigPtr;
            if (sig.paramTypes.size() > 1) {
                r_.ok = false;
                r_.error = "handler '" + reg.regName + "' has " +
                           std::to_string(sig.paramTypes.size()) +
                           " input parameters; a handler is a pure Functor "
                           "with at most one input — aggregate into a record";
                return;
            }
            Handler h;
            h.name = reg.regName;
            if (!sig.paramTypes.empty()) {
                auto in = resolveType(sig.paramTypes[0]);
                if (!in) {
                    r_.ok = false;
                    r_.error = "handler '" + reg.regName +
                               "' input type '" + sig.paramTypes[0] +
                               "' is not a stdlib scalar or a recognised "
                               "record aggregate";
                    return;
                }
                h.in_type = *in;
            }
            auto out = resolveType(sig.returnType);
            if (!out) {
                r_.ok = false;
                r_.error = "handler '" + reg.regName + "' output type '" +
                           sig.returnType +
                           "' is not a stdlib scalar or a recognised record "
                           "aggregate (a void return is not a valid Functor "
                           "Out)";
                return;
            }
            h.out_type = *out;
            g.handlers().push_back(std::move(h));
        }

        if (g.handlers().empty()) {
            r_.ok = false;
            r_.error =
                "no handler registrations found in the analyzed source";
            return;
        }

        if (flow_.has_value()) {
            const auto& [fname, stages] = *flow_;
            Flow f;
            f.name = fname;
            for (std::size_t i = 0; i + 1 < stages.size(); ++i)
                for (const auto& s : stages[i])
                    for (const auto& t : stages[i + 1])
                        f.edges.push_back(Edge{s, t});
            if (!stages.empty())
                for (const auto& s : stages.back())
                    f.edges.push_back(Edge{s, std::nullopt});
            g.set_flow(std::move(f));
        }

        r_.graph = std::move(g);
        r_.ok = true;
    }
};

}  // namespace

Result analyzeAstJson(const std::string& astJson) {
    Result r;
    json tu;
    try {
        tu = json::parse(astJson);
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = std::string("clang AST JSON parse failed: ") + e.what();
        return r;
    }
    if (!tu.is_object()) {
        r.ok = false;
        r.error = "clang AST JSON root is not an object";
        return r;
    }
    Analyzer a(r);
    try {
        a.run(tu);
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = std::string("AST walk failed: ") + e.what();
    }
    return r;
}

// The user translation unit is wrapped in a sentinel namespace so a
// single `-ast-dump-filter` selects exactly the user's declarations.
// Without it, `clang -ast-dump=json` serialises the AST of every libc++
// header the source pulls in — on the order of 600 MB of JSON for a file
// that merely `#include <string>`. With the wrap the dump is ~100 KB and
// sub-second. `#include` lines are hoisted above the namespace: an
// `#include` inside a namespace would wrongly nest the included file's
// declarations. Other preprocessor lines (`#define`, `#if`) are
// token/macro level and stay in place. Known limitation: an `#include`
// inside a block comment or an inactive `#if 0` is hoisted regardless —
// pathological in a topo-app registration file.
static constexpr const char* kWrapNamespace = "topo_app_static_user_tu";

static std::string wrapSourceForFilteredDump(const std::string& source) {
    std::string includes;
    std::string body;
    std::size_t i = 0;
    while (i < source.size()) {
        std::size_t nl = source.find('\n', i);
        std::size_t end = (nl == std::string::npos) ? source.size() : nl + 1;
        std::string line = source.substr(i, end - i);
        std::size_t p = line.find_first_not_of(" \t");
        bool isInclude = false;
        if (p != std::string::npos && line[p] == '#') {
            ++p;
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
                ++p;
            isInclude = line.compare(p, 7, "include") == 0;
        }
        (isInclude ? includes : body) += line;
        i = end;
    }
    std::string out = includes;
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += "namespace ";
    out += kWrapNamespace;
    out += " {\n";
    out += body;
    out += "\n}  // namespace ";
    out += kWrapNamespace;
    out += '\n';
    return out;
}

Result analyzeFile(const std::string& clangBinary,
                   const std::string& sourcePath,
                   const std::vector<std::string>& extraArgs) {
    Result r;

    // Read the user source, wrap it (see wrapSourceForFilteredDump), and
    // write the wrapped form to a temp file — the dump runs on that.
    std::string source;
    {
        std::ifstream in(sourcePath, std::ios::binary);
        if (!in) {
            r.ok = false;
            r.error = "cannot read source file '" + sourcePath + "'";
            return r;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        source = ss.str();
    }

    // Wrapped-temp collision fix: the
    // previous implementation derived the temp filename from the input
    // source's stem (`<stem>.topo-app-static-wrapped.cpp`), which two
    // sibling sources with the same basename — a routine layout, e.g.
    // `a/main.cpp` + `b/main.cpp` or multiple crates' `lib.cpp` — would
    // share. Under CppDriver's concurrent dispatch (std::async per
    // compile job) that meant two analyzeFile() calls writing the same
    // path and the clang subprocess reading whichever content landed
    // last — silent miscompile of the declared `.topo`. The migration
    // mirrors the sibling fix for the preprocessor scanner: route
    // through `topo::platform::TempFile`, which probes for a
    // collision-free `<stem>-<pid>-<counter>.<ext>` and creates the
    // file atomically (O_CREAT|O_EXCL on POSIX, CREATE_NEW on Windows)
    // before any subprocess sees the path.
    topo::platform::TempFile tempWrapped("topo-app-static-wrapped", ".cpp");
    const std::filesystem::path& wrappedPath = tempWrapped.path();
    {
        std::ofstream out(wrappedPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            r.ok = false;
            r.error = "cannot write wrapped temp source '" +
                      wrappedPath.string() + "'";
            return r;
        }
        out << wrapSourceForFilteredDump(source);
    }

    // clang -Xclang -ast-dump=json -Xclang -ast-dump-filter=<ns> ...
    // The filter restricts the JSON to the sentinel namespace's subtree
    // (the user's declarations) — see wrapSourceForFilteredDump.
    std::vector<std::string> args = {
        "-Xclang", "-ast-dump=json",
        "-Xclang", std::string("-ast-dump-filter=") + kWrapNamespace,
        "-fsyntax-only", "-std=c++20", "-xc++"};

    // macOS: resolve the active SDK via xcrun so libc++ headers
    // (<string>, <vector>, ...) are found — the same handling
    // topo-extract-cpp performs. On Linux / Windows the host toolchain's
    // default search paths plus caller-supplied -I cover the stdlib.
#ifdef __APPLE__
    {
        platform::CapturedProcessResult sdk =
            platform::runProcessCapture("xcrun", {"--show-sdk-path"});
        if (sdk.exitCode == 0) {
            std::string p = sdk.stdoutOutput;
            while (!p.empty() &&
                   (p.back() == '\n' || p.back() == '\r' || p.back() == ' '))
                p.pop_back();
            if (!p.empty()) {
                args.push_back("-isysroot");
                args.push_back(p);
                args.push_back("-stdlib=libc++");
            }
        }
    }
#endif

    // The wrapped source sits in the temp dir; -I the original source's
    // directory so the user's quoted / relative `#include "..."` still
    // resolve from where they were authored.
    {
        std::filesystem::path srcDir =
            std::filesystem::path(sourcePath).parent_path();
        args.push_back("-I");
        args.push_back(srcDir.empty() ? std::string(".") : srcDir.string());
    }
    for (const auto& a : extraArgs) args.push_back(a);
    args.push_back(wrappedPath.string());

    // A generous timeout: a single small translation unit dumps fast,
    // but a cold filesystem / large header set can be slow. 60 s mirrors
    // the toolchain's other clang-subprocess ceilings.
    platform::CapturedProcessResult res =
        platform::runProcessCaptureWithTimeout(clangBinary, args,
                                               /*timeoutMs=*/60'000);

    if (res.stdoutOutput.empty()) {
        r.ok = false;
        r.error = "clang produced no AST dump (exit " +
                  std::to_string(res.exitCode) + ")";
        if (!res.stderrOutput.empty())
            r.error += ":\n" + res.stderrOutput;
        return r;
    }

    r = analyzeAstJson(res.stdoutOutput);
    // A clang parse error (non-zero exit) with a still-usable partial AST
    // is surfaced as a degraded result rather than a hard failure: the
    // dump is best-effort, but a fatal parse error that yielded nothing
    // already returned above.
    if (r.ok && res.exitCode != 0) {
        r.fidelity = Fidelity::Degraded;
        r.unsupported.push_back(
            "clang reported diagnostics during the parse (exit " +
            std::to_string(res.exitCode) +
            "); the AST may be incomplete — review the source");
    }
    return r;
}

}  // namespace topo::app::staticfe
