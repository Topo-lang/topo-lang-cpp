// topo-extract-cpp: libclang-based C++ function body extractor
//
// Reads a JSON request from stdin, uses libclang to parse each listed source
// file, traverses the AST to find requested functions, converts their bodies
// to TranspileModel nodes, and writes the result as TranspileModule JSON to
// stdout.
//
// Protocol:
//   stdin  <- {"files": [...], "functions": [...], "symbolTable": {...},
//              "includePaths": [...]   // optional, forwarded as -I
//              "cxxStandard": "c++17"  // optional, defaults to c++20
//             }
//   stdout -> {"types": [], "functions": [...]}
//
// `cxxStandard` controls the `-std=` flag libclang receives. The default
// is `c++20` so concepts (`template <Concept T>`, `requires` clauses)
// keep parsing as before; a caller whose project pins
// `[build].standard = "c++17"` can pass it through here to avoid silent
// skew between topo-check-time parsing and build-time parsing.

#include <clang-c/Index.h>
#include <nlohmann/json.hpp>

#include "topo/Platform/Process.h"
#include "topo/Platform/ToolResolution.h"
#include "topo/Transpile/TranspileModel.h"
#include "topo/Transpile/TranspileModelJson.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
using namespace topo::transpile;
using topo::OwnershipKind;
using topo::Parameter;
using topo::TypeNode;

// ===================================================================
// Helpers: CXString RAII + utility
// ===================================================================

static std::string cxStr(CXString s) {
    const char* cstr = clang_getCString(s);
    std::string result = cstr ? cstr : "";
    clang_disposeString(s);
    return result;
}

// ===================================================================
// Qualified name reconstruction from cursor hierarchy
// ===================================================================

static std::string getQualifiedName(CXCursor cursor) {
    std::string name;
    CXCursor parent = clang_getCursorSemanticParent(cursor);
    CXCursorKind parentKind = clang_getCursorKind(parent);
    if (parentKind != CXCursor_TranslationUnit && parentKind != CXCursor_InvalidFile) {
        name = getQualifiedName(parent) + "::";
    }
    name += cxStr(clang_getCursorSpelling(cursor));
    return name;
}

// ===================================================================
// Type extraction: CXType -> TypeNode
// ===================================================================

static TypeNode extractType(CXType type) {
    TypeNode node;

    // Peel qualifiers
    if (clang_isConstQualifiedType(type)) {
        node.isConst = true;
    }

    // Handle pointer / lvalue-reference
    if (type.kind == CXType_Pointer) {
        node.modifier = TypeNode::Ptr;
        CXType pointee = clang_getPointeeType(type);
        node = extractType(pointee);
        node.modifier = TypeNode::Ptr;
        return node;
    }
    if (type.kind == CXType_LValueReference) {
        node.modifier = TypeNode::Ref;
        CXType pointee = clang_getPointeeType(type);
        node = extractType(pointee);
        node.modifier = TypeNode::Ref;
        return node;
    }

    // Smart-pointer ownership recovery.
    //
    // The Rust/C++ emitters expect a single TypeNode whose nameParts hold the
    // *pointee* type and whose `ownership` carries the wrapper kind (the
    // emitter clears ownership and re-emits the inner node). Only the three
    // exact std owning/weak wrappers are recognized — any other template
    // (std::optional, std::vector, user templates, ...) falls through to the
    // unchanged spelling path below, so this stays conservative and never
    // misclassifies a non-ownership generic. Match is on the canonical type's
    // spelling prefix because typedef/elaborated forms still canonicalize to
    // `std::unique_ptr<...>` / `std::__1::unique_ptr<...>` (libstdc++ /
    // libc++ inline-namespace), so we also accept an embedded "::unique_ptr<"
    // segment.
    {
        CXType canonical = clang_getCanonicalType(type);
        std::string canonSpelling = cxStr(clang_getTypeSpelling(canonical));
        auto matchesStdTemplate = [&](const char* shortName) -> bool {
            std::string stdForm = std::string("std::") + shortName + "<";
            if (canonSpelling.rfind(stdForm, 0) == 0) return true;
            // Inline-namespace forms, e.g. std::__1::unique_ptr< .
            std::string scoped = std::string("::") + shortName + "<";
            return canonSpelling.rfind("std::", 0) == 0 &&
                   canonSpelling.find(scoped) != std::string::npos;
        };
        OwnershipKind owned = OwnershipKind::None;
        if (matchesStdTemplate("unique_ptr"))
            owned = OwnershipKind::Owned;
        else if (matchesStdTemplate("shared_ptr"))
            owned = OwnershipKind::Shared;
        else if (matchesStdTemplate("weak_ptr"))
            owned = OwnershipKind::Weak;

        if (owned != OwnershipKind::None &&
            clang_Type_getNumTemplateArguments(canonical) >= 1) {
            CXType elem = clang_Type_getTemplateArgumentAsType(canonical, 0);
            if (elem.kind != CXType_Invalid) {
                bool wasConst = node.isConst;
                node = extractType(elem);
                // The wrapper itself, not the pointee, may be const-qualified
                // (`const std::unique_ptr<Foo>`); preserve that.
                if (wasConst) node.isConst = true;
                node.ownership = owned;
                return node;
            }
        }
    }

    // Get the canonical spelling and split on "::"
    std::string spelling = cxStr(clang_getTypeSpelling(type));

    // Strip leading "const " / trailing " &" / " *" from the spelling
    // (these are already handled by modifier/isConst above)
    auto stripPrefix = [](std::string& s, const std::string& prefix) {
        if (s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix) s = s.substr(prefix.size());
    };
    auto stripSuffix = [](std::string& s, const std::string& suffix) {
        if (s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix)
            s = s.substr(0, s.size() - suffix.size());
    };

    stripPrefix(spelling, "const ");
    stripSuffix(spelling, " &");
    stripSuffix(spelling, " *");

    // Trim whitespace
    while (!spelling.empty() && spelling.back() == ' ')
        spelling.pop_back();

    // Split on "::" into nameParts
    if (spelling.empty()) {
        node.nameParts.push_back("void");
    } else {
        size_t pos = 0;
        while (pos < spelling.size()) {
            size_t next = spelling.find("::", pos);
            if (next == std::string::npos) {
                node.nameParts.push_back(spelling.substr(pos));
                break;
            }
            node.nameParts.push_back(spelling.substr(pos, next - pos));
            pos = next + 2;
        }
    }

    return node;
}

// ===================================================================
// Token-based operator extraction for BinaryOperator / UnaryOperator
// ===================================================================

struct ExtractContext {
    CXTranslationUnit tu;
};

// Get source text for a cursor's extent
static std::string getSourceText(const ExtractContext& ctx, CXCursor cursor) {
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(ctx.tu, range, &tokens, &numTokens);

    std::string result;
    for (unsigned i = 0; i < numTokens; ++i) {
        if (i > 0) result += " ";
        result += cxStr(clang_getTokenSpelling(ctx.tu, tokens[i]));
    }
    clang_disposeTokens(ctx.tu, tokens, numTokens);
    return result;
}

// Collect immediate children of a cursor
static std::vector<CXCursor> getChildren(CXCursor cursor) {
    std::vector<CXCursor> children;
    clang_visitChildren(
        cursor,
        [](CXCursor c, CXCursor, CXClientData data) {
            auto* vec = static_cast<std::vector<CXCursor>*>(data);
            vec->push_back(c);
            return CXChildVisit_Continue;
        },
        &children);
    return children;
}

// Recover declaration-site generic type parameters from a class/function
// template cursor.
//
// MVP scope: bare type-parameter NAMES only. Anything richer (non-type
// params `<int N>`, template-template params, a constrained/defaulted type
// param) cannot be faithfully round-tripped yet, so it is NOT added to
// templateParams; instead the caller is told (via the out-params) to record
// a human-readable note and downgrade fidelity. A constrained or defaulted
// type param is deliberately dropped entirely rather than emitted bare,
// because emitting `<typename T>` for a declaration that semantically said
// `<typename T = int>` (or with a concept bound) would silently widen the
// contract — a conservative omission is safer than a misleading emission.
//
// Parse the default-type clause of a TemplateTypeParameter cursor by
// tokenising its source extent. libclang does not expose default-type via a
// stable child-cursor shape (built-in scalars like `int` produce no
// referenced cursor at all), so the text-based fallback handles
// `typename T = X` uniformly across primitives (`int`), qualified types
// (`std::string`), and bound + default (`Concept T = X`). Returns the
// qualified-name parts in source order (`std :: string` → `["std","string"]`)
// or empty if no `=` token is found / the right-hand-side is non-trivial
// (template-args, parenthesised expressions, etc.). The extractor's
// translation unit is passed via the same `clang_tokenize` API used by
// existing token-based helpers; the cursor extent is bounded so we never
// stray past the parameter into sibling tokens.
static std::vector<std::string>
extractDefaultTokens(CXTranslationUnit tu, CXCursor paramCursor) {
    std::vector<std::string> out;
    CXSourceRange range = clang_getCursorExtent(paramCursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, range, &tokens, &numTokens);
    if (!tokens || numTokens == 0) {
        if (tokens) clang_disposeTokens(tu, tokens, numTokens);
        return out;
    }
    // Locate the `=` punctuation token; everything before it is the
    // parameter declaration (typename + name + optional concept), everything
    // after is the default expression.
    unsigned eqIdx = numTokens;
    for (unsigned i = 0; i < numTokens; ++i) {
        if (clang_getTokenKind(tokens[i]) != CXToken_Punctuation) continue;
        std::string s = cxStr(clang_getTokenSpelling(tu, tokens[i]));
        if (s == "=") { eqIdx = i; break; }
    }
    if (eqIdx == numTokens) {
        clang_disposeTokens(tu, tokens, numTokens);
        return out;
    }
    // Walk the right-hand-side. Accept identifier tokens (`int`, `string`,
    // `MyType`) joined by `::` punctuation. Reject any other punctuation
    // (`<`, `(`, `*`, `&`, `[`) — those signal template instantiations or
    // pointer/array forms outside this MVP, and we'd rather drop the
    // default than emit a misleading one. Stop at the cursor extent's end
    // (no need to chase `,` or `>` because the cursor extent is per-param).
    for (unsigned i = eqIdx + 1; i < numTokens; ++i) {
        CXTokenKind k = clang_getTokenKind(tokens[i]);
        std::string s = cxStr(clang_getTokenSpelling(tu, tokens[i]));
        if (k == CXToken_Identifier || k == CXToken_Keyword) {
            out.push_back(s);
        } else if (k == CXToken_Punctuation && s == "::") {
            // separator between qualified-name segments; already implied
            // by sequential identifier tokens, so no token to append.
        } else {
            // Comma, `>`, `<`, parens, etc. — bail to conservative drop.
            out.clear();
            break;
        }
    }
    clang_disposeTokens(tu, tokens, numTokens);
    return out;
}

// Tokenise a NonTypeTemplateParameter cursor and recover its default literal
// expression as a source-spelling string. The C++ MVP intentionally accepts
// only three literal forms — integer literal (`= 10`, `= 0x1F`), bool
// literal (`= true`, `= false`), and a single identifier/keyword treated as
// an enum literal spelling (`= Color::Red` → "Color::Red"). Anything more
// complex (`= N+1`, `= sizeof(T)`, parenthesised expressions, function
// calls, template instantiations) returns the empty string so the caller
// can record a conservative downgrade. Returning the source spelling rather
// than a parsed value keeps the wire format language-agnostic; the spelling
// is emitted verbatim by CppEmitter.
//
// Mirrors `extractDefaultTokens`' token-walk pattern: locate the `=`
// punctuation, walk the right-hand side until the cursor extent ends,
// accept only literal-shaped tokens, bail to empty on anything else.
static std::string
extractDefaultLiteralValue(CXTranslationUnit tu, CXCursor paramCursor) {
    std::string out;
    CXSourceRange range = clang_getCursorExtent(paramCursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, range, &tokens, &numTokens);
    if (!tokens || numTokens == 0) {
        if (tokens) clang_disposeTokens(tu, tokens, numTokens);
        return out;
    }
    auto cleanup = [&]() { clang_disposeTokens(tu, tokens, numTokens); };
    unsigned eqIdx = numTokens;
    for (unsigned i = 0; i < numTokens; ++i) {
        if (clang_getTokenKind(tokens[i]) != CXToken_Punctuation) continue;
        std::string s = cxStr(clang_getTokenSpelling(tu, tokens[i]));
        if (s == "=") { eqIdx = i; break; }
    }
    if (eqIdx == numTokens) { cleanup(); return out; }
    // Pass 1: classify the right-hand side shape.
    //
    // Accepted shapes (literal-only MVP):
    //   1. Single literal token (integer literal like `10`, `0x1F`, `1u`,
    //      or character literal). CXToken_Literal covers all of these.
    //   2. Single keyword token `true` / `false` (bool literals are
    //      CXToken_Keyword, not CXToken_Literal).
    //   3. A `::`-joined chain of identifiers (`Color::Red`, `ns::E::Val`)
    //      — surfaced as the enum literal-spelling. A bare single
    //      identifier is also accepted (unscoped enumerator).
    //   4. A leading sign `-` or `+` followed by a single integer literal
    //      (`= -1`, `= +0`). This is the only non-trivial punctuation
    //      sequence we tolerate; anything else (`*`, `&`, `(`, `<`) bails.
    //
    // Anything else — operators, parens, template-args, multiple literal
    // tokens, mixed identifier+literal — drops to the empty string so the
    // caller surfaces a conservative downgrade.
    std::vector<std::pair<CXTokenKind, std::string>> rhs;
    for (unsigned i = eqIdx + 1; i < numTokens; ++i) {
        CXTokenKind k = clang_getTokenKind(tokens[i]);
        std::string s = cxStr(clang_getTokenSpelling(tu, tokens[i]));
        rhs.emplace_back(k, std::move(s));
    }
    cleanup();
    if (rhs.empty()) return out;

    auto isBoolKeyword = [](const std::string& s) {
        return s == "true" || s == "false";
    };
    auto isIdent = [](CXTokenKind k) {
        return k == CXToken_Identifier || k == CXToken_Keyword;
    };

    // Shape 1 + 2 + leading-sign integer.
    if (rhs.size() == 1) {
        const auto& [k, s] = rhs[0];
        if (k == CXToken_Literal) { return s; }
        if (k == CXToken_Keyword && isBoolKeyword(s)) { return s; }
        if (k == CXToken_Identifier) { return s; } // unscoped enumerator
    }
    if (rhs.size() == 2 && rhs[0].first == CXToken_Punctuation &&
        (rhs[0].second == "-" || rhs[0].second == "+") &&
        rhs[1].first == CXToken_Literal) {
        return rhs[0].second + rhs[1].second;
    }
    // Shape 3: qualified-name chain `A::B::C`. Identifier (`::` Identifier)+
    if (rhs.size() >= 3 && (rhs.size() % 2) == 1) {
        bool wellFormed = isIdent(rhs[0].first);
        std::string joined = rhs[0].second;
        for (size_t i = 1; i + 1 < rhs.size() && wellFormed; i += 2) {
            const auto& sep = rhs[i];
            const auto& nxt = rhs[i + 1];
            if (sep.first != CXToken_Punctuation || sep.second != "::") {
                wellFormed = false;
                break;
            }
            if (!isIdent(nxt.first)) { wellFormed = false; break; }
            joined += "::";
            joined += nxt.second;
        }
        if (wellFormed) return joined;
    }
    return out;
}

// Scan the templated entity's tokens for a top-level `requires` clause of the
// form `requires Concept<T> && Concept<T> ...` and append each
// ConceptSpecialization to the matching param's `extraBounds`. libclang
// exposes no direct requires-clause API, so this walks the tokens between
// the `template <...>` header and the body / trailing semicolon, mirroring
// the text-fallback pattern already used by `extractDefaultTokens`.
//
// Strict / conservative: any form not matching "top-level `&&` chain of
// `Concept<T>` calls on previously declared type params" leaves `outParams`
// untouched and pushes one downgrade note. The wire contract stays clean —
// either every concept in the chain attaches, or none does.
static void attachRequiresClauseBounds(CXTranslationUnit tu,
                                        CXCursor templateCursor,
                                        std::vector<topo::TemplateParamDecl>& outParams,
                                        std::vector<std::string>& outNotes) {
    if (outParams.empty()) return;

    CXSourceRange range = clang_getCursorExtent(templateCursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, range, &tokens, &numTokens);
    if (!tokens || numTokens == 0) {
        if (tokens) clang_disposeTokens(tu, tokens, numTokens);
        return;
    }

    auto cleanup = [&]() { clang_disposeTokens(tu, tokens, numTokens); };
    auto tokKind = [&](unsigned i) { return clang_getTokenKind(tokens[i]); };
    auto tokText = [&](unsigned i) {
        return cxStr(clang_getTokenSpelling(tu, tokens[i]));
    };

    // Skip the leading `template <...>` clause. The first non-whitespace token
    // must be the `template` keyword for a CXCursor_FunctionTemplate /
    // CXCursor_ClassTemplate. Locate the matching `>` that closes the
    // parameter list by depth-counting `<` / `>` (ignoring `<=`/`>=`/`<<`/`>>`,
    // which never appear inside a well-formed template-parameter list).
    unsigned i = 0;
    while (i < numTokens && tokKind(i) != CXToken_Keyword) ++i;
    if (i >= numTokens || tokText(i) != "template") { cleanup(); return; }
    ++i; // past `template`
    while (i < numTokens && !(tokKind(i) == CXToken_Punctuation && tokText(i) == "<")) ++i;
    if (i >= numTokens) { cleanup(); return; }
    int depth = 1;
    ++i;
    while (i < numTokens && depth > 0) {
        if (tokKind(i) == CXToken_Punctuation) {
            const std::string s = tokText(i);
            if (s == "<") ++depth;
            else if (s == ">") --depth;
        }
        ++i;
    }
    if (depth != 0 || i >= numTokens) { cleanup(); return; }

    // After the closing `>` of the template-parameter list, the next token may
    // be `requires` (the requires clause) or a declaration introducer
    // (`struct`/`class`/`template`/return-type identifier). Skip nothing — if
    // it isn't `requires`, no clause exists and we're done without touching
    // outParams.
    if (i >= numTokens ||
        !(tokKind(i) == CXToken_Keyword && tokText(i) == "requires")) {
        cleanup();
        return;
    }
    ++i; // past `requires`

    // Parse the requires-clause as a `&&`-conjoined sequence of
    // `Concept<T>` calls. The clause body is bounded by the next
    // declaration introducer that cannot legally appear inside a top-level
    // concept-specialization expression: `{` (body), `;` (forward decl),
    // `struct`/`class`/`union`/`enum`/`typename`/`auto` keywords, or another
    // declarator keyword. Encountering anything outside the recognised
    // grammar aborts the upgrade.
    auto isClauseTerminator = [&](unsigned k) {
        if (k >= numTokens) return true;
        CXTokenKind tk = tokKind(k);
        if (tk == CXToken_Punctuation) {
            const std::string s = tokText(k);
            if (s == "{" || s == ";") return true;
        }
        if (tk == CXToken_Keyword) {
            const std::string s = tokText(k);
            if (s == "struct" || s == "class" || s == "union" || s == "enum" ||
                s == "auto") return true;
        }
        return false;
    };

    // Collected `(targetParamIndex, conceptNameParts)` entries. We only
    // commit them to outParams[*].extraBounds after the whole clause has
    // parsed cleanly — half-formed clauses leave the model untouched.
    struct StagedBound {
        size_t paramIdx;
        std::vector<std::string> conceptNameParts;
    };
    std::vector<StagedBound> staged;

    auto findParamByName = [&](const std::string& n) -> int {
        for (size_t k = 0; k < outParams.size(); ++k) {
            if (outParams[k].kind == topo::TemplateParamDecl::TypeParam &&
                outParams[k].name == n) {
                return static_cast<int>(k);
            }
        }
        return -1;
    };

    while (!isClauseTerminator(i)) {
        // Parse one `Concept<T>` chunk:
        //   Identifier (:: Identifier)* < Identifier >
        std::vector<std::string> conceptParts;
        // First identifier
        if (tokKind(i) != CXToken_Identifier && tokKind(i) != CXToken_Keyword) {
            outNotes.push_back(
                "requires-clause: unrecognised token before concept name; "
                "all bounds dropped");
            cleanup();
            return;
        }
        conceptParts.push_back(tokText(i));
        ++i;
        while (i + 1 < numTokens && tokKind(i) == CXToken_Punctuation &&
               tokText(i) == "::" &&
               (tokKind(i + 1) == CXToken_Identifier ||
                tokKind(i + 1) == CXToken_Keyword)) {
            conceptParts.push_back(tokText(i + 1));
            i += 2;
        }

        // `<` opens the concept's template-argument list.
        if (i >= numTokens || tokKind(i) != CXToken_Punctuation ||
            tokText(i) != "<") {
            outNotes.push_back(
                "requires-clause: concept '" + conceptParts.back() +
                "' missing '<T>' argument; all bounds dropped");
            cleanup();
            return;
        }
        ++i;
        if (i >= numTokens || tokKind(i) != CXToken_Identifier) {
            outNotes.push_back(
                "requires-clause: concept '" + conceptParts.back() +
                "' argument is not a bare type name; all bounds dropped");
            cleanup();
            return;
        }
        const std::string argName = tokText(i);
        ++i;
        if (i >= numTokens || tokKind(i) != CXToken_Punctuation ||
            tokText(i) != ">") {
            outNotes.push_back(
                "requires-clause: concept '" + conceptParts.back() +
                "' has multi-argument or complex form; all bounds dropped");
            cleanup();
            return;
        }
        ++i;

        // Map argName to a declared type param. Anything else (free
        // identifier, multi-arg form, expression operand) drops the whole
        // clause to avoid silent widening.
        int idx = findParamByName(argName);
        if (idx < 0) {
            outNotes.push_back(
                "requires-clause: concept arg '" + argName +
                "' does not name a declared type param; all bounds dropped");
            cleanup();
            return;
        }
        staged.push_back(StagedBound{static_cast<size_t>(idx),
                                     std::move(conceptParts)});

        // Either the clause ends here, or `&&` introduces the next concept.
        if (isClauseTerminator(i)) break;
        if (!(tokKind(i) == CXToken_Punctuation && tokText(i) == "&&")) {
            outNotes.push_back(
                "requires-clause: expected '&&' between concepts, got '" +
                tokText(i) + "'; all bounds dropped");
            cleanup();
            return;
        }
        ++i;
    }
    cleanup();

    if (staged.empty()) return;

    // Commit: for each staged bound, prepend to the matching param's bound
    // list. If a param already carries a `constraintType` (constrained-form
    // single bound from `template <Concept T>`), append the new concepts as
    // `extraBounds`. Otherwise the first concept becomes `constraintType` and
    // the remainder go to `extraBounds`.
    for (const auto& sb : staged) {
        topo::TypeNode tn;
        tn.nameParts = sb.conceptNameParts;
        auto& p = outParams[sb.paramIdx];
        if (p.constraintType.nameParts.empty()) {
            p.constraintType = std::move(tn);
        } else {
            p.extraBounds.push_back(std::move(tn));
        }
    }
}

// True iff the cursor's source extent contains a `...` ellipsis token.
// libclang exposes no direct "is this template param a pack" API, so a
// variadic type param (`typename... Ts`) is recognised by token-scanning
// the parameter extent for the `...` punctuation token — the same
// text-fallback pattern `extractDefaultTokens` already uses.
static bool cursorExtentHasEllipsis(CXTranslationUnit tu, CXCursor cursor) {
    CXSourceRange r = clang_getCursorExtent(cursor);
    CXToken* toks = nullptr;
    unsigned nToks = 0;
    clang_tokenize(tu, r, &toks, &nToks);
    bool found = false;
    if (toks) {
        for (unsigned i = 0; i < nToks; ++i) {
            if (clang_getTokenKind(toks[i]) == CXToken_Punctuation) {
                std::string s = cxStr(clang_getTokenSpelling(tu, toks[i]));
                if (s == "...") {
                    found = true;
                    break;
                }
            }
        }
        clang_disposeTokens(tu, toks, nToks);
    }
    return found;
}

// `outParams` receives the recovered bare type params in source order.
// `outNotes` receives one human-readable entry per downgraded construct.
static void collectTemplateParams(CXTranslationUnit tu,
                                   CXCursor templateCursor,
                                   std::vector<topo::TemplateParamDecl>& outParams,
                                   std::vector<std::string>& outNotes) {
    for (const auto& child : getChildren(templateCursor)) {
        CXCursorKind ck = clang_getCursorKind(child);
        std::string name = cxStr(clang_getCursorSpelling(child));

        if (ck == CXCursor_TemplateTypeParameter) {
            // A bare type param has no child cursors. A constraint
            // (concept/`requires`) or a default type surfaces as child
            // cursors (TypeRef/TemplateRef/expression) under the param.
            //
            // Recognised forms (anything else downgrades to bare):
            //   1. single TemplateRef           → `template <Concept T> ...`
            //      (C++20 constrained-parameter; concept captured as `bound`)
            //   2. single TypeRef               → `template <typename T = X> ...`
            //      (C++17 default; type captured as `default`)
            //   3. TemplateRef then TypeRef     → `template <Concept T = X> ...`
            //      (constrained + default; both captured)
            //
            // The walker that recovers the qualified name from a TemplateRef
            // (concept declaration) is also reused for TypeRef → default type
            // (type declaration). For built-in scalar types (`int`, `double`)
            // a TypeRef has no referenced cursor; fall back to the type
            // spelling via `clang_getCursorType` so primitive defaults still
            // round-trip cleanly.
            auto qualifiedFromRef = [](CXCursor ref) {
                std::vector<std::string> parts;
                if (clang_Cursor_isNull(ref)) return parts;
                std::vector<std::string> rev;
                CXCursor cur = ref;
                while (!clang_Cursor_isNull(cur)) {
                    CXCursorKind ck2 = clang_getCursorKind(cur);
                    if (ck2 == CXCursor_TranslationUnit ||
                        ck2 == CXCursor_InvalidFile) {
                        break;
                    }
                    std::string n = cxStr(clang_getCursorSpelling(cur));
                    if (!n.empty()) rev.push_back(n);
                    cur = clang_getCursorSemanticParent(cur);
                }
                for (auto it = rev.rbegin(); it != rev.rend(); ++it)
                    parts.push_back(*it);
                return parts;
            };
            auto qualifiedFromTemplateRef = [&](CXCursor templateRef) {
                std::vector<std::string> parts =
                    qualifiedFromRef(clang_getCursorReferenced(templateRef));
                if (parts.empty()) {
                    std::string n = cxStr(clang_getCursorSpelling(templateRef));
                    if (!n.empty()) parts.push_back(n);
                }
                return parts;
            };
            auto kids = getChildren(child);
            // Concept bound is read from the single TemplateRef child cursor
            // (the C++20 constrained-parameter form). Anything else among
            // the children is ignored for bound recovery — bound is reserved
            // for explicit concept references.
            std::vector<std::string> boundParts;
            if (!kids.empty() &&
                clang_getCursorKind(kids[0]) == CXCursor_TemplateRef) {
                boundParts = qualifiedFromTemplateRef(kids[0]);
            }
            // Default is read by tokenising the parameter extent and walking
            // tokens past `=`. Works uniformly for built-in scalars (no
            // TypeRef child cursor) and user-declared types.
            std::vector<std::string> defaultParts =
                extractDefaultTokens(tu, child);

            if (!boundParts.empty() || !defaultParts.empty()) {
                topo::TemplateParamDecl p;
                p.kind = topo::TemplateParamDecl::TypeParam;
                p.name = name;
                if (!boundParts.empty()) p.constraintType.nameParts = boundParts;
                if (!defaultParts.empty()) {
                    topo::TypeNode def;
                    def.nameParts = defaultParts;
                    p.defaultType = std::move(def);
                }
                outParams.push_back(std::move(p));
                continue;
            }
            // No bound / no parseable default. If the parameter still has
            // child cursors (e.g. complex requires-clause, template-template,
            // an unrecognised default expression) downgrade to bare and note;
            // otherwise just emit bare.
            if (!kids.empty()) {
                outNotes.push_back(
                    "constrained or defaulted type parameter '" + name +
                    "' downgraded to plain (bound/default dropped)");
                continue;
            }
            topo::TemplateParamDecl p;
            p.kind = topo::TemplateParamDecl::TypeParam;
            p.name = name;
            // Variadic pack `typename... Ts`: a parameter pack carries no
            // child cursors (same shape as a bare type param), so it is
            // recognised by token-scanning the extent for the `...`
            // ellipsis. The kind stays TypeParam — `isVariadic` is an
            // orthogonal flag — and the source stays at full fidelity.
            if (cursorExtentHasEllipsis(tu, child)) {
                p.isVariadic = true;
            }
            outParams.push_back(std::move(p));
        } else if (ck == CXCursor_NonTypeTemplateParameter) {
            // `template <int N>`, `template <std::size_t N>`, `template <int N = 10>`.
            // Capture as kind="nontype" with constraintType carrying the
            // value type (the cursor's CXType gives the canonical spelling,
            // matching how `extractDefaultTokens` recovers built-in scalars
            // when no referenced cursor exists). Defaults are recovered by
            // token-scanning the cursor extent (literal-only — integer /
            // bool / enum literal-spelling); complex constant expressions
            // (`= N+1`, `= sizeof(T)`, template instantiations) drop the
            // default with a downgrade note while still emitting the bare
            // parameter. Template template forms / parameter packs stay
            // outside the MVP and downgrade entirely.
            auto kids = getChildren(child);
            // First detect whether a `=` default clause is present at all.
            // This disambiguates "default-value expression children"
            // (IntegerLiteral / UnexposedExpr / DeclRefExpr — kept as a
            // bare param, default recovered by the literal helper) from a
            // genuine variadic / template-template form (no `=`, but
            // unexpected children — dropped entirely).
            const bool hasEqToken = [&]() {
                CXSourceRange r = clang_getCursorExtent(child);
                CXToken* toks = nullptr;
                unsigned nToks = 0;
                clang_tokenize(tu, r, &toks, &nToks);
                bool eq = false;
                if (toks) {
                    for (unsigned i = 0; i < nToks; ++i) {
                        if (clang_getTokenKind(toks[i]) ==
                            CXToken_Punctuation) {
                            std::string s =
                                cxStr(clang_getTokenSpelling(tu, toks[i]));
                            if (s == "=") { eq = true; break; }
                        }
                    }
                    clang_disposeTokens(tu, toks, nToks);
                }
                return eq;
            }();
            // Literal-only default recovery (integer / bool / enum literal-
            // spelling). Returns empty for complex constant expressions.
            std::string defaultLiteral =
                extractDefaultLiteralValue(tu, child);
            const bool oneTypeRef =
                kids.size() == 1 &&
                clang_getCursorKind(kids[0]) == CXCursor_TypeRef;
            const bool allTypeRefs = !kids.empty() && [&]() {
                for (const auto& k : kids) {
                    if (clang_getCursorKind(k) != CXCursor_TypeRef)
                        return false;
                }
                return true;
            }();
            // Drop the param entirely only when there is NO `=` clause yet
            // still unexpected children — that is the variadic /
            // template-template shape, outside the MVP. When a `=` clause
            // is present, the children belong to the default expression;
            // we keep the bare param and (below) either capture the
            // literal default or surface a non-literal-default downgrade.
            if (!hasEqToken && !kids.empty() && !oneTypeRef && !allTypeRefs) {
                outNotes.push_back(
                    "non-type template parameter '" + name +
                    "' has unrecognised children (variadic/template-template); dropped");
                continue;
            }
            CXType valueType = clang_getCursorType(child);
            std::string spelling = cxStr(clang_getTypeSpelling(valueType));
            if (spelling.empty()) {
                outNotes.push_back(
                    "non-type template parameter '" + name +
                    "' has unknown value type; dropped");
                continue;
            }
            topo::TemplateParamDecl p;
            p.kind = topo::TemplateParamDecl::NonTypeParam;
            p.name = name;
            // `std::size_t` arrives as a single nameParts entry — the C++
            // emitter will join multi-segment paths with `::` later if we
            // ever decide to qualify-split this spelling.
            p.constraintType.nameParts.push_back(std::move(spelling));
            if (!defaultLiteral.empty()) {
                p.defaultValue = std::move(defaultLiteral);
            } else if (hasEqToken) {
                // `=` clause present but the right-hand side is not a
                // literal (`= N+1`, `= sizeof(T)`, template instantiation):
                // conservative drop of the default, bare param kept.
                outNotes.push_back(
                    "non-type template parameter '" + name +
                    "' has non-literal default expression; default dropped (bare param kept)");
            }
            outParams.push_back(std::move(p));
        } else if (ck == CXCursor_TemplateTemplateParameter) {
            // Template-template parameter (`template <template<typename>
            // class C> ...`). MVP recognises only the canonical
            // `template<typename> class X` shape: every inner cursor must
            // be a plain `CXCursor_TemplateTypeParameter` carrying no bound
            // and no default. The inner type params are captured into
            // `innerParams`; the outer param itself is kind="template".
            //
            // Range-out conservative drop (note + nothing emitted):
            //   - nested template-template (an inner cursor is itself a
            //     `CXCursor_TemplateTemplateParameter`)
            //   - non-type inner params (`CXCursor_NonTypeTemplateParameter`)
            //   - an inner type param carrying a bound / default (it would
            //     surface child cursors under the inner cursor)
            auto innerKids = getChildren(child);
            bool innerOk = !innerKids.empty();
            std::vector<topo::TemplateParamDecl> innerParams;
            for (const auto& ik : innerKids) {
                CXCursorKind ikk = clang_getCursorKind(ik);
                if (ikk != CXCursor_TemplateTypeParameter) {
                    // Non-type inner param / nested template-template /
                    // anything else → out of MVP range.
                    innerOk = false;
                    break;
                }
                // A plain inner type param has no child cursors. Any child
                // cursor means a bound / default — out of MVP range.
                if (!getChildren(ik).empty()) {
                    innerOk = false;
                    break;
                }
                topo::TemplateParamDecl ip;
                ip.kind = topo::TemplateParamDecl::TypeParam;
                // The inner param of `template<typename> class C` is
                // unnamed; keep whatever spelling libclang reports (empty
                // for the canonical form).
                ip.name = cxStr(clang_getCursorSpelling(ik));
                innerParams.push_back(std::move(ip));
            }
            if (!innerOk) {
                outNotes.push_back(
                    "template template parameter '" + name +
                    "' has nested/non-type/constrained inner params "
                    "(outside MVP); dropped");
            } else {
                topo::TemplateParamDecl p;
                p.kind = topo::TemplateParamDecl::TemplateTemplateParam;
                p.name = name;
                p.innerParams = std::move(innerParams);
                outParams.push_back(std::move(p));
            }
        }
        // Other child kinds (the templated struct/class/function body,
        // base specifiers, etc.) are not template params — ignore here.
    }

    // After the params loop, look for a trailing `requires`-clause of the
    // form `requires A<T> && B<T> ...` and graduate matching params from
    // single-bound to multi-bound. libclang exposes no requires-clause API
    // so the helper tokenises the templated entity's extent.
    attachRequiresClauseBounds(tu, templateCursor, outParams, outNotes);
}

// Find the operator token between two child extents for BinaryOperator.
// Returns the operator text (e.g. "+", "==", "&&").
static std::string findBinaryOperatorToken(const ExtractContext& ctx, CXCursor binaryOp) {
    auto children = getChildren(binaryOp);
    if (children.size() < 2) return "?";

    // Get end of LHS and start of RHS
    CXSourceRange lhsRange = clang_getCursorExtent(children[0]);
    CXSourceRange rhsRange = clang_getCursorExtent(children[1]);
    CXSourceLocation lhsEnd = clang_getRangeEnd(lhsRange);
    CXSourceLocation rhsStart = clang_getRangeStart(rhsRange);

    // Create a range between the two children
    CXSourceRange gapRange = clang_getRange(lhsEnd, rhsStart);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(ctx.tu, gapRange, &tokens, &numTokens);

    std::string op = "?";
    for (unsigned i = 0; i < numTokens; ++i) {
        CXTokenKind tkind = clang_getTokenKind(tokens[i]);
        if (tkind == CXToken_Punctuation) {
            op = cxStr(clang_getTokenSpelling(ctx.tu, tokens[i]));
            break;
        }
        if (tkind == CXToken_Keyword) {
            // Handle C++ operator keywords (and, or, not)
            std::string kw = cxStr(clang_getTokenSpelling(ctx.tu, tokens[i]));
            if (kw == "and" || kw == "or") {
                op = kw;
                break;
            }
        }
    }
    clang_disposeTokens(ctx.tu, tokens, numTokens);
    return op;
}

// Find the operator token for a UnaryOperator
static std::string findUnaryOperatorToken(const ExtractContext& ctx, CXCursor unaryOp) {
    CXSourceRange range = clang_getCursorExtent(unaryOp);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(ctx.tu, range, &tokens, &numTokens);

    std::string op = "?";
    if (numTokens > 0) {
        // The operator is typically the first or last punctuation token
        std::string first = cxStr(clang_getTokenSpelling(ctx.tu, tokens[0]));
        if (first == "!" || first == "-" || first == "~" || first == "not") {
            op = first;
        } else if (first == "++") {
            op = "pre++";
        } else if (first == "--") {
            op = "pre--";
        } else if (numTokens > 1) {
            // Postfix: operator at the end
            std::string last = cxStr(clang_getTokenSpelling(ctx.tu, tokens[numTokens - 1]));
            if (last == "++") {
                op = "post++";
            } else if (last == "--") {
                op = "post--";
            }
        }
    }
    clang_disposeTokens(ctx.tu, tokens, numTokens);
    return op;
}

// Map operator token text to BinaryOp enum
static bool mapBinaryOp(const std::string& opText, BinaryOp& out) {
    if (opText == "+") {
        out = BinaryOp::Add;
        return true;
    }
    if (opText == "-") {
        out = BinaryOp::Sub;
        return true;
    }
    if (opText == "*") {
        out = BinaryOp::Mul;
        return true;
    }
    if (opText == "/") {
        out = BinaryOp::Div;
        return true;
    }
    if (opText == "%") {
        out = BinaryOp::Mod;
        return true;
    }
    if (opText == "==") {
        out = BinaryOp::Eq;
        return true;
    }
    if (opText == "!=") {
        out = BinaryOp::NotEq;
        return true;
    }
    if (opText == "<") {
        out = BinaryOp::Less;
        return true;
    }
    if (opText == ">") {
        out = BinaryOp::Greater;
        return true;
    }
    if (opText == "<=") {
        out = BinaryOp::LessEq;
        return true;
    }
    if (opText == ">=") {
        out = BinaryOp::GreaterEq;
        return true;
    }
    if (opText == "&&" || opText == "and") {
        out = BinaryOp::And;
        return true;
    }
    if (opText == "||" || opText == "or") {
        out = BinaryOp::Or;
        return true;
    }
    if (opText == "&") {
        out = BinaryOp::BitAnd;
        return true;
    }
    if (opText == "|") {
        out = BinaryOp::BitOr;
        return true;
    }
    if (opText == "^") {
        out = BinaryOp::BitXor;
        return true;
    }
    if (opText == "<<") {
        out = BinaryOp::Shl;
        return true;
    }
    if (opText == ">>") {
        out = BinaryOp::Shr;
        return true;
    }
    return false;
}

// ===================================================================
// Forward declarations
// ===================================================================

static ExprPtr convertExpr(const ExtractContext& ctx, CXCursor cursor);
static StmtPtr convertStmt(const ExtractContext& ctx, CXCursor cursor);
static std::vector<StmtPtr> convertCompoundBody(const ExtractContext& ctx, CXCursor compound);

// ===================================================================
// Expression conversion
// ===================================================================

// Make an UnsupportedExpr with a description
static ExprPtr makeUnsupported(const std::string& desc) {
    auto e = std::make_unique<UnsupportedExpr>();
    e->fidelity = Fidelity::Source;
    e->description = desc;
    return e;
}

// Get source location as "line:col" for diagnostics
static std::string locationStr(CXCursor cursor) {
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    unsigned line = 0, col = 0;
    clang_getSpellingLocation(loc, nullptr, &line, &col, nullptr);
    return "line " + std::to_string(line) + ":" + std::to_string(col);
}

static ExprPtr convertExpr(const ExtractContext& ctx, CXCursor cursor) {
    CXCursorKind kind = clang_getCursorKind(cursor);

    switch (kind) {
    // --- Integer / floating / string / bool literals ---
    case CXCursor_IntegerLiteral: {
        auto e = std::make_unique<LiteralExpr>();
        e->fidelity = Fidelity::Source;
        e->litKind = LiteralKind::Integer;
        e->value = getSourceText(ctx, cursor);
        return e;
    }
    case CXCursor_FloatingLiteral: {
        auto e = std::make_unique<LiteralExpr>();
        e->fidelity = Fidelity::Source;
        e->litKind = LiteralKind::Float;
        e->value = getSourceText(ctx, cursor);
        return e;
    }
    case CXCursor_StringLiteral: {
        auto e = std::make_unique<LiteralExpr>();
        e->fidelity = Fidelity::Source;
        e->litKind = LiteralKind::String;
        e->value = getSourceText(ctx, cursor);
        return e;
    }
    case CXCursor_CXXBoolLiteralExpr: {
        auto e = std::make_unique<LiteralExpr>();
        e->fidelity = Fidelity::Source;
        e->litKind = LiteralKind::Boolean;
        e->value = getSourceText(ctx, cursor);
        return e;
    }
    case CXCursor_CharacterLiteral: {
        auto e = std::make_unique<LiteralExpr>();
        e->fidelity = Fidelity::Source;
        e->litKind = LiteralKind::String;
        e->value = getSourceText(ctx, cursor);
        return e;
    }

    // --- Variable / declaration references ---
    case CXCursor_DeclRefExpr: {
        auto e = std::make_unique<VarRefExpr>();
        e->fidelity = Fidelity::Source;
        e->name = cxStr(clang_getCursorSpelling(cursor));
        return e;
    }

    // --- Member access (obj.member or obj->member) ---
    case CXCursor_MemberRefExpr: {
        auto e = std::make_unique<MemberAccessExpr>();
        e->fidelity = Fidelity::Source;
        e->member = cxStr(clang_getCursorSpelling(cursor));

        auto children = getChildren(cursor);
        if (!children.empty()) {
            e->object = convertExpr(ctx, children[0]);
        } else {
            // Implicit this->member: represent as VarRef("this")
            auto thisRef = std::make_unique<VarRefExpr>();
            thisRef->fidelity = Fidelity::Source;
            thisRef->name = "this";
            e->object = std::move(thisRef);
        }
        return e;
    }

    // --- Array subscript ---
    case CXCursor_ArraySubscriptExpr: {
        auto children = getChildren(cursor);
        if (children.size() >= 2) {
            auto e = std::make_unique<IndexExpr>();
            e->fidelity = Fidelity::Source;
            e->object = convertExpr(ctx, children[0]);
            e->index = convertExpr(ctx, children[1]);
            return e;
        }
        return makeUnsupported("incomplete array subscript at " + locationStr(cursor));
    }

    // --- Function / method calls ---
    case CXCursor_CallExpr: {
        auto e = std::make_unique<CallExpr>();
        e->fidelity = Fidelity::Source;

        auto children = getChildren(cursor);
        // First child is the callee expression; remaining are arguments.
        // For simple calls the callee is a DeclRefExpr; for member calls it's
        // a MemberRefExpr. We extract the qualified name from the referenced
        // declaration.
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_getCursorKind(referenced) != CXCursor_InvalidFile &&
            clang_getCursorKind(referenced) != CXCursor_NoDeclFound) {
            e->callee = getQualifiedName(referenced);
        } else if (!children.empty()) {
            e->callee = cxStr(clang_getCursorSpelling(children[0]));
        } else {
            e->callee = "<unknown>";
        }

        // Arguments: skip the first child (callee expression)
        for (size_t i = 1; i < children.size(); ++i) {
            e->args.push_back(convertExpr(ctx, children[i]));
        }
        return e;
    }

    // --- Binary operators ---
    case CXCursor_BinaryOperator: {
        auto children = getChildren(cursor);
        if (children.size() < 2) {
            return makeUnsupported("binary operator with < 2 children at " + locationStr(cursor));
        }

        std::string opText = findBinaryOperatorToken(ctx, cursor);

        // Check for assignment (produces AssignStmt at the statement level;
        // at expression level we still model it as UnsupportedExpr since the
        // model separates assignment into AssignStmt).
        if (opText == "=") {
            // Return a special marker — the statement converter will handle it.
            // At expression level, represent as unsupported.
            return makeUnsupported("assignment expression at " + locationStr(cursor));
        }

        // Compound assignment operators (+=, -=, etc.) -> CompoundAssignExpr
        {
            BinaryOp compoundOp;
            bool isCompound = false;
            if (opText == "+=") {
                compoundOp = BinaryOp::Add;
                isCompound = true;
            } else if (opText == "-=") {
                compoundOp = BinaryOp::Sub;
                isCompound = true;
            } else if (opText == "*=") {
                compoundOp = BinaryOp::Mul;
                isCompound = true;
            } else if (opText == "/=") {
                compoundOp = BinaryOp::Div;
                isCompound = true;
            } else if (opText == "%=") {
                compoundOp = BinaryOp::Mod;
                isCompound = true;
            } else if (opText == "&=") {
                compoundOp = BinaryOp::BitAnd;
                isCompound = true;
            } else if (opText == "|=") {
                compoundOp = BinaryOp::BitOr;
                isCompound = true;
            } else if (opText == "^=") {
                compoundOp = BinaryOp::BitXor;
                isCompound = true;
            } else if (opText == "<<=") {
                compoundOp = BinaryOp::Shl;
                isCompound = true;
            } else if (opText == ">>=") {
                compoundOp = BinaryOp::Shr;
                isCompound = true;
            }

            if (isCompound) {
                auto e = std::make_unique<CompoundAssignExpr>();
                e->fidelity = Fidelity::Source;
                e->op = compoundOp;
                e->target = convertExpr(ctx, children[0]);
                e->value = convertExpr(ctx, children[1]);
                return e;
            }
        }

        BinaryOp op;
        if (!mapBinaryOp(opText, op)) {
            return makeUnsupported("unsupported binary operator '" + opText + "' at " + locationStr(cursor));
        }

        auto e = std::make_unique<BinaryOpExpr>();
        e->fidelity = Fidelity::Source;
        e->op = op;
        e->lhs = convertExpr(ctx, children[0]);
        e->rhs = convertExpr(ctx, children[1]);
        return e;
    }

    // --- Unary operators ---
    case CXCursor_UnaryOperator: {
        auto children = getChildren(cursor);
        if (children.empty()) {
            return makeUnsupported("unary operator with no operand at " + locationStr(cursor));
        }

        std::string opText = findUnaryOperatorToken(ctx, cursor);
        auto e = std::make_unique<UnaryOpExpr>();
        e->fidelity = Fidelity::Source;
        e->operand = convertExpr(ctx, children[0]);

        if (opText == "-") {
            e->op = UnaryOp::Negate;
        } else if (opText == "!" || opText == "not") {
            e->op = UnaryOp::Not;
        } else if (opText == "~") {
            e->op = UnaryOp::BitNot;
        } else if (opText == "pre++") {
            e->op = UnaryOp::PreIncrement;
        } else if (opText == "post++") {
            e->op = UnaryOp::PostIncrement;
        } else if (opText == "pre--") {
            e->op = UnaryOp::PreDecrement;
        } else if (opText == "post--") {
            e->op = UnaryOp::PostDecrement;
        } else {
            return makeUnsupported("unsupported unary operator '" + opText + "' at " + locationStr(cursor));
        }
        return e;
    }

    // --- new / construct ---
    // NOTE: CXCursor_CXXConstructExpr is unavailable in older libclang versions
    // (e.g., Homebrew LLVM 18). CXCursor_CXXNewExpr alone handles new-expressions;
    // construct expressions in other contexts fall through to the default handler.
    case CXCursor_CXXNewExpr: {
        auto e = std::make_unique<ConstructExpr>();
        e->fidelity = Fidelity::Source;
        CXType ty = clang_getCursorType(cursor);
        e->type = extractType(ty);

        auto children = getChildren(cursor);
        for (const auto& child : children) {
            e->args.push_back(convertExpr(ctx, child));
        }
        return e;
    }

    // --- Parenthesized expression: unwrap ---
    case CXCursor_ParenExpr: {
        auto children = getChildren(cursor);
        if (!children.empty()) {
            return convertExpr(ctx, children[0]);
        }
        return makeUnsupported("empty paren expr at " + locationStr(cursor));
    }

    // --- Implicit cast / explicit cast: unwrap child ---
    case CXCursor_CStyleCastExpr:
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXDynamicCastExpr:
    case CXCursor_CXXReinterpretCastExpr:
    case CXCursor_CXXConstCastExpr:
    case CXCursor_CXXFunctionalCastExpr: {
        // For casts, extract the child expression (the operand)
        auto children = getChildren(cursor);
        if (!children.empty()) {
            return convertExpr(ctx, children.back());
        }
        return makeUnsupported("cast expression at " + locationStr(cursor));
    }

    // --- Unexposed expression (often wraps implicit casts) ---
    case CXCursor_UnexposedExpr: {
        auto children = getChildren(cursor);
        if (!children.empty()) {
            return convertExpr(ctx, children[0]);
        }
        return makeUnsupported("unexposed expression at " + locationStr(cursor));
    }

    // --- CXCursor_CXXThisExpr ---
    case CXCursor_CXXThisExpr: {
        auto e = std::make_unique<VarRefExpr>();
        e->fidelity = Fidelity::Source;
        e->name = "this";
        return e;
    }

    // --- CXCursor_CXXNullPtrLiteralExpr ---
    case CXCursor_CXXNullPtrLiteralExpr: {
        auto e = std::make_unique<LiteralExpr>();
        e->fidelity = Fidelity::Source;
        e->litKind = LiteralKind::Integer;
        e->value = "nullptr";
        return e;
    }

    // --- Conditional (ternary) operator ---
    case CXCursor_ConditionalOperator: {
        auto children = getChildren(cursor);
        if (children.size() >= 3) {
            auto e = std::make_unique<TernaryExpr>();
            e->fidelity = Fidelity::Source;
            e->condition = convertExpr(ctx, children[0]);
            e->trueExpr = convertExpr(ctx, children[1]);
            e->falseExpr = convertExpr(ctx, children[2]);
            return e;
        }
        return makeUnsupported("ternary operator with insufficient children at " + locationStr(cursor));
    }

    default: break;
    }

    // Fallback: unsupported expression
    std::string kindSpelling = cxStr(clang_getCursorKindSpelling(kind));
    return makeUnsupported(kindSpelling + " at " + locationStr(cursor));
}

// ===================================================================
// Statement conversion
// ===================================================================

static StmtPtr convertStmt(const ExtractContext& ctx, CXCursor cursor) {
    CXCursorKind kind = clang_getCursorKind(cursor);

    switch (kind) {
    // --- Compound statement: should not be reached at top level; recurse ---
    case CXCursor_CompoundStmt: {
        // This is called when a compound statement appears as a child
        // (e.g. if body). The caller should use convertCompoundBody instead.
        // Handle gracefully by returning the first statement.
        auto stmts = convertCompoundBody(ctx, cursor);
        if (stmts.size() == 1) {
            return std::move(stmts[0]);
        }
        // Multiple statements in an unexpected compound: wrap first one
        // (this path should rarely be hit)
        if (!stmts.empty()) {
            return std::move(stmts[0]);
        }
        // Empty compound: return a no-op ExprStmt
        auto s = std::make_unique<ExprStmt>();
        s->fidelity = Fidelity::Source;
        s->expr = makeUnsupported("empty compound statement");
        return s;
    }

    // --- Variable declaration ---
    case CXCursor_VarDecl: {
        auto s = std::make_unique<VarDeclStmt>();
        s->fidelity = Fidelity::Source;
        s->name = cxStr(clang_getCursorSpelling(cursor));
        s->type = extractType(clang_getCursorType(cursor));

        // Check for initializer
        auto children = getChildren(cursor);
        for (const auto& child : children) {
            CXCursorKind ck = clang_getCursorKind(child);
            // Skip TypeRef children; first expression child is the initializer
            if (ck != CXCursor_TypeRef && ck != CXCursor_TemplateRef && ck != CXCursor_NamespaceRef) {
                s->init = convertExpr(ctx, child);
                break;
            }
        }
        return s;
    }

    // --- Declaration statement (wraps VarDecl) ---
    case CXCursor_DeclStmt: {
        auto children = getChildren(cursor);
        if (children.size() == 1) {
            return convertStmt(ctx, children[0]);
        }
        // Multiple declarations in one statement: convert each
        // Return the first one (simplification)
        if (!children.empty()) {
            return convertStmt(ctx, children[0]);
        }
        auto s = std::make_unique<ExprStmt>();
        s->fidelity = Fidelity::Source;
        s->expr = makeUnsupported("empty declaration statement at " + locationStr(cursor));
        return s;
    }

    // --- Return statement ---
    case CXCursor_ReturnStmt: {
        auto s = std::make_unique<ReturnStmt>();
        s->fidelity = Fidelity::Source;
        auto children = getChildren(cursor);
        if (!children.empty()) {
            s->value = convertExpr(ctx, children[0]);
        }
        return s;
    }

    // --- If statement ---
    case CXCursor_IfStmt: {
        auto s = std::make_unique<IfStmt>();
        s->fidelity = Fidelity::Source;
        auto children = getChildren(cursor);
        // libclang IfStmt children: [condition, then-body, else-body(optional)]
        if (children.size() >= 1) {
            s->condition = convertExpr(ctx, children[0]);
        }
        if (children.size() >= 2) {
            if (clang_getCursorKind(children[1]) == CXCursor_CompoundStmt) {
                s->thenBody = convertCompoundBody(ctx, children[1]);
            } else {
                s->thenBody.push_back(convertStmt(ctx, children[1]));
            }
        }
        if (children.size() >= 3) {
            if (clang_getCursorKind(children[2]) == CXCursor_CompoundStmt) {
                s->elseBody = convertCompoundBody(ctx, children[2]);
            } else {
                s->elseBody.push_back(convertStmt(ctx, children[2]));
            }
        }
        return s;
    }

    // --- For statement ---
    case CXCursor_ForStmt: {
        auto s = std::make_unique<ForStmt>();
        s->fidelity = Fidelity::Source;
        auto children = getChildren(cursor);
        // libclang ForStmt children ordering:
        //   [init, condition, increment, body]
        // Some may be null/missing for empty clauses.
        // With libclang, null clauses are simply absent.
        // We need to distinguish based on cursor kinds.
        //
        // Approach: iterate children and categorize.
        // - DeclStmt/VarDecl at position 0 -> init
        // - Last CompoundStmt -> body
        // - Other expressions in between -> condition, increment
        if (!children.empty()) {
            size_t idx = 0;

            // Init: first child if it's a DeclStmt or certain expression
            if (idx < children.size()) {
                CXCursorKind ck = clang_getCursorKind(children[idx]);
                if (ck == CXCursor_DeclStmt || ck == CXCursor_VarDecl) {
                    s->init = convertStmt(ctx, children[idx]);
                    ++idx;
                } else if (ck == CXCursor_BinaryOperator) {
                    // Could be assignment as init
                    // Check if it's the last item (then it's the body)
                    if (children.size() > 1) {
                        // Treat as expression statement init
                        auto initStmt = std::make_unique<ExprStmt>();
                        initStmt->fidelity = Fidelity::Source;
                        initStmt->expr = convertExpr(ctx, children[idx]);
                        s->init = std::move(initStmt);
                        ++idx;
                    }
                }
            }

            // Body is always the last child (CompoundStmt or single stmt)
            size_t bodyIdx = children.size() - 1;

            // Condition and increment are between init and body
            if (idx < bodyIdx) {
                s->condition = convertExpr(ctx, children[idx]);
                ++idx;
            }
            if (idx < bodyIdx) {
                s->increment = convertExpr(ctx, children[idx]);
                ++idx;
            }

            // Body
            if (clang_getCursorKind(children[bodyIdx]) == CXCursor_CompoundStmt) {
                s->body = convertCompoundBody(ctx, children[bodyIdx]);
            } else {
                s->body.push_back(convertStmt(ctx, children[bodyIdx]));
            }
        }
        return s;
    }

    // --- While statement ---
    case CXCursor_WhileStmt: {
        auto s = std::make_unique<WhileStmt>();
        s->fidelity = Fidelity::Source;
        auto children = getChildren(cursor);
        // children: [condition, body]
        if (children.size() >= 1) {
            s->condition = convertExpr(ctx, children[0]);
        }
        if (children.size() >= 2) {
            if (clang_getCursorKind(children[1]) == CXCursor_CompoundStmt) {
                s->body = convertCompoundBody(ctx, children[1]);
            } else {
                s->body.push_back(convertStmt(ctx, children[1]));
            }
        }
        return s;
    }

    // --- Binary operator at statement level: check for assignment ---
    case CXCursor_BinaryOperator: {
        auto children = getChildren(cursor);
        if (children.size() >= 2) {
            std::string opText = findBinaryOperatorToken(ctx, cursor);
            if (opText == "=") {
                auto s = std::make_unique<AssignStmt>();
                s->fidelity = Fidelity::Source;
                s->target = convertExpr(ctx, children[0]);
                s->value = convertExpr(ctx, children[1]);
                return s;
            }
        }
        // Non-assignment binary op as a statement: wrap in ExprStmt
        auto s = std::make_unique<ExprStmt>();
        s->fidelity = Fidelity::Source;
        s->expr = convertExpr(ctx, cursor);
        return s;
    }

    // --- Break statement ---
    case CXCursor_BreakStmt: {
        auto s = std::make_unique<BreakStmt>();
        s->fidelity = Fidelity::Source;
        return s;
    }

    // --- Continue statement ---
    case CXCursor_ContinueStmt: {
        auto s = std::make_unique<ContinueStmt>();
        s->fidelity = Fidelity::Source;
        return s;
    }

    // --- Switch statement ---
    case CXCursor_SwitchStmt: {
        auto s = std::make_unique<SwitchStmt>();
        s->fidelity = Fidelity::Source;
        auto children = getChildren(cursor);
        // libclang SwitchStmt children: [subject-expr, CompoundStmt-body]
        if (!children.empty()) {
            s->subject = convertExpr(ctx, children[0]);
        }
        // The body is a CompoundStmt containing CaseStmt and DefaultStmt nodes
        if (children.size() >= 2) {
            auto bodyChildren = getChildren(children[1]);
            SwitchCase* currentCase = nullptr;
            for (const auto& child : bodyChildren) {
                CXCursorKind ck = clang_getCursorKind(child);
                if (ck == CXCursor_CaseStmt) {
                    s->cases.emplace_back();
                    currentCase = &s->cases.back();
                    auto caseChildren = getChildren(child);
                    // CaseStmt children: [value-expr, body-stmt...]
                    if (!caseChildren.empty()) {
                        currentCase->value = convertExpr(ctx, caseChildren[0]);
                    }
                    // Remaining children are the body statements
                    for (size_t i = 1; i < caseChildren.size(); ++i) {
                        CXCursorKind cck = clang_getCursorKind(caseChildren[i]);
                        if (cck == CXCursor_CaseStmt || cck == CXCursor_DefaultStmt) {
                            // Nested case: handle as a new case at the switch level
                            // (fall-through pattern) - skip here, will be handled later
                        } else if (cck == CXCursor_CompoundStmt) {
                            auto body = convertCompoundBody(ctx, caseChildren[i]);
                            for (auto& st : body) {
                                currentCase->body.push_back(std::move(st));
                            }
                        } else {
                            currentCase->body.push_back(convertStmt(ctx, caseChildren[i]));
                        }
                    }
                } else if (ck == CXCursor_DefaultStmt) {
                    s->cases.emplace_back();
                    currentCase = &s->cases.back();
                    // value stays nullptr for default case
                    auto defaultChildren = getChildren(child);
                    for (const auto& dc : defaultChildren) {
                        if (clang_getCursorKind(dc) == CXCursor_CompoundStmt) {
                            auto body = convertCompoundBody(ctx, dc);
                            for (auto& st : body) {
                                currentCase->body.push_back(std::move(st));
                            }
                        } else {
                            currentCase->body.push_back(convertStmt(ctx, dc));
                        }
                    }
                } else if (currentCase) {
                    // Statement within a case (non-compound fallthrough style)
                    currentCase->body.push_back(convertStmt(ctx, child));
                }
            }
        }
        return s;
    }

    // --- Do statement (approximate as WhileStmt with doWhile flag) ---
    case CXCursor_DoStmt: {
        auto s = std::make_unique<WhileStmt>();
        s->fidelity = Fidelity::Source;
        auto children = getChildren(cursor);
        // libclang DoStmt children: [body, condition]
        if (children.size() >= 2) {
            if (clang_getCursorKind(children[0]) == CXCursor_CompoundStmt) {
                s->body = convertCompoundBody(ctx, children[0]);
            } else {
                s->body.push_back(convertStmt(ctx, children[0]));
            }
            s->condition = convertExpr(ctx, children[1]);
        }
        return s;
    }

    // --- Compound assignment at statement level ---
    case CXCursor_CompoundAssignOperator: {
        auto children = getChildren(cursor);
        if (children.size() >= 2) {
            std::string opText = findBinaryOperatorToken(ctx, cursor);
            BinaryOp compoundOp;
            bool isCompound = false;
            if (opText == "+=") {
                compoundOp = BinaryOp::Add;
                isCompound = true;
            } else if (opText == "-=") {
                compoundOp = BinaryOp::Sub;
                isCompound = true;
            } else if (opText == "*=") {
                compoundOp = BinaryOp::Mul;
                isCompound = true;
            } else if (opText == "/=") {
                compoundOp = BinaryOp::Div;
                isCompound = true;
            } else if (opText == "%=") {
                compoundOp = BinaryOp::Mod;
                isCompound = true;
            } else if (opText == "&=") {
                compoundOp = BinaryOp::BitAnd;
                isCompound = true;
            } else if (opText == "|=") {
                compoundOp = BinaryOp::BitOr;
                isCompound = true;
            } else if (opText == "^=") {
                compoundOp = BinaryOp::BitXor;
                isCompound = true;
            } else if (opText == "<<=") {
                compoundOp = BinaryOp::Shl;
                isCompound = true;
            } else if (opText == ">>=") {
                compoundOp = BinaryOp::Shr;
                isCompound = true;
            }

            if (isCompound) {
                auto s = std::make_unique<ExprStmt>();
                s->fidelity = Fidelity::Source;
                auto e = std::make_unique<CompoundAssignExpr>();
                e->fidelity = Fidelity::Source;
                e->op = compoundOp;
                e->target = convertExpr(ctx, children[0]);
                e->value = convertExpr(ctx, children[1]);
                s->expr = std::move(e);
                return s;
            }
        }
        // Fallback
        auto s = std::make_unique<ExprStmt>();
        s->fidelity = Fidelity::Source;
        s->expr = convertExpr(ctx, cursor);
        return s;
    }

    // --- Expression as statement: any other expression kind ---
    case CXCursor_CallExpr:
    case CXCursor_CXXNewExpr:
    case CXCursor_UnaryOperator:
    case CXCursor_CXXDeleteExpr: {
        auto s = std::make_unique<ExprStmt>();
        s->fidelity = Fidelity::Source;
        s->expr = convertExpr(ctx, cursor);
        return s;
    }

    default: break;
    }

    // Fallback: wrap as ExprStmt with UnsupportedExpr
    auto s = std::make_unique<ExprStmt>();
    s->fidelity = Fidelity::Source;
    std::string kindSpelling = cxStr(clang_getCursorKindSpelling(kind));
    s->expr = makeUnsupported(kindSpelling + " at " + locationStr(cursor));
    return s;
}

// ===================================================================
// Compound body: convert all children of a CompoundStmt
// ===================================================================

static std::vector<StmtPtr> convertCompoundBody(const ExtractContext& ctx, CXCursor compound) {
    std::vector<StmtPtr> stmts;
    auto children = getChildren(compound);
    for (const auto& child : children) {
        stmts.push_back(convertStmt(ctx, child));
    }
    return stmts;
}

// ===================================================================
// Function extraction: convert a function definition to TranspileFunction
// ===================================================================

static TranspileFunction convertFunction(const ExtractContext& ctx,
                                         CXCursor funcCursor,
                                         const std::string& qualifiedName) {
    TranspileFunction fn;
    fn.qualifiedName = qualifiedName;
    fn.fidelity = Fidelity::Source;

    // Return type
    CXType resultType = clang_getCursorResultType(funcCursor);
    fn.returnType = extractType(resultType);

    // Parameters
    // clang_Cursor_getNumArguments returns -1 for CXCursor_FunctionTemplate;
    // fall back to walking children for ParmDecl cursors so member function
    // templates and free function templates produce the same param shape as
    // regular functions.
    int numArgs = clang_Cursor_getNumArguments(funcCursor);
    if (numArgs >= 0) {
        for (int i = 0; i < numArgs; ++i) {
            CXCursor argCursor = clang_Cursor_getArgument(funcCursor, i);
            Parameter param;
            param.name = cxStr(clang_getCursorSpelling(argCursor));
            param.type = extractType(clang_getCursorType(argCursor));
            fn.params.push_back(std::move(param));
        }
    } else {
        for (const auto& child : getChildren(funcCursor)) {
            if (clang_getCursorKind(child) != CXCursor_ParmDecl) continue;
            Parameter param;
            param.name = cxStr(clang_getCursorSpelling(child));
            param.type = extractType(clang_getCursorType(child));
            fn.params.push_back(std::move(param));
        }
    }

    // Body: find the CompoundStmt child
    auto children = getChildren(funcCursor);
    for (const auto& child : children) {
        if (clang_getCursorKind(child) == CXCursor_CompoundStmt) {
            fn.body = convertCompoundBody(ctx, child);
            break;
        }
    }

    // Collect unsupported markers from the body
    // Walk all statements and expressions looking for UnsupportedExpr nodes
    std::function<void(const Stmt&)> collectUnsupported;
    std::function<void(const Expr&)> collectUnsupportedExpr;

    collectUnsupportedExpr = [&](const Expr& e) {
        if (e.kind() == Expr::Kind::Unsupported) {
            fn.unsupported.push_back(static_cast<const UnsupportedExpr&>(e).description);
        }
        // Recurse into sub-expressions
        switch (e.kind()) {
        case Expr::Kind::BinaryOp: {
            const auto& be = static_cast<const BinaryOpExpr&>(e);
            if (be.lhs) collectUnsupportedExpr(*be.lhs);
            if (be.rhs) collectUnsupportedExpr(*be.rhs);
            break;
        }
        case Expr::Kind::UnaryOp: {
            const auto& ue = static_cast<const UnaryOpExpr&>(e);
            if (ue.operand) collectUnsupportedExpr(*ue.operand);
            break;
        }
        case Expr::Kind::Call: {
            const auto& ce = static_cast<const CallExpr&>(e);
            for (const auto& a : ce.args)
                if (a) collectUnsupportedExpr(*a);
            break;
        }
        case Expr::Kind::MemberAccess: {
            const auto& me = static_cast<const MemberAccessExpr&>(e);
            if (me.object) collectUnsupportedExpr(*me.object);
            break;
        }
        case Expr::Kind::Index: {
            const auto& ie = static_cast<const IndexExpr&>(e);
            if (ie.object) collectUnsupportedExpr(*ie.object);
            if (ie.index) collectUnsupportedExpr(*ie.index);
            break;
        }
        case Expr::Kind::Construct: {
            const auto& ce = static_cast<const ConstructExpr&>(e);
            for (const auto& a : ce.args)
                if (a) collectUnsupportedExpr(*a);
            break;
        }
        case Expr::Kind::Lambda: {
            const auto& le = static_cast<const LambdaExpr&>(e);
            for (const auto& st : le.body)
                if (st) collectUnsupported(*st);
            break;
        }
        case Expr::Kind::Throw: {
            const auto& te = static_cast<const ThrowExpr&>(e);
            if (te.operand) collectUnsupportedExpr(*te.operand);
            break;
        }
        case Expr::Kind::Ternary: {
            const auto& te = static_cast<const TernaryExpr&>(e);
            if (te.condition) collectUnsupportedExpr(*te.condition);
            if (te.trueExpr) collectUnsupportedExpr(*te.trueExpr);
            if (te.falseExpr) collectUnsupportedExpr(*te.falseExpr);
            break;
        }
        case Expr::Kind::CompoundAssign: {
            const auto& ca = static_cast<const CompoundAssignExpr&>(e);
            if (ca.target) collectUnsupportedExpr(*ca.target);
            if (ca.value) collectUnsupportedExpr(*ca.value);
            break;
        }
        default: break;
        }
    };

    collectUnsupported = [&](const Stmt& s) {
        switch (s.kind()) {
        case Stmt::Kind::VarDecl: {
            const auto& vd = static_cast<const VarDeclStmt&>(s);
            if (vd.init) collectUnsupportedExpr(*vd.init);
            break;
        }
        case Stmt::Kind::Assign: {
            const auto& as = static_cast<const AssignStmt&>(s);
            if (as.target) collectUnsupportedExpr(*as.target);
            if (as.value) collectUnsupportedExpr(*as.value);
            break;
        }
        case Stmt::Kind::Return: {
            const auto& rs = static_cast<const ReturnStmt&>(s);
            if (rs.value) collectUnsupportedExpr(*rs.value);
            break;
        }
        case Stmt::Kind::If: {
            const auto& is = static_cast<const IfStmt&>(s);
            if (is.condition) collectUnsupportedExpr(*is.condition);
            for (const auto& st : is.thenBody)
                collectUnsupported(*st);
            for (const auto& st : is.elseBody)
                collectUnsupported(*st);
            break;
        }
        case Stmt::Kind::For: {
            const auto& fs = static_cast<const ForStmt&>(s);
            if (fs.init) collectUnsupported(*fs.init);
            if (fs.condition) collectUnsupportedExpr(*fs.condition);
            if (fs.increment) collectUnsupportedExpr(*fs.increment);
            for (const auto& st : fs.body)
                collectUnsupported(*st);
            break;
        }
        case Stmt::Kind::While: {
            const auto& ws = static_cast<const WhileStmt&>(s);
            if (ws.condition) collectUnsupportedExpr(*ws.condition);
            for (const auto& st : ws.body)
                collectUnsupported(*st);
            break;
        }
        case Stmt::Kind::ExprStmt: {
            const auto& es = static_cast<const ExprStmt&>(s);
            if (es.expr) collectUnsupportedExpr(*es.expr);
            break;
        }
        case Stmt::Kind::TryCatch: {
            const auto& tc = static_cast<const TryCatchStmt&>(s);
            for (const auto& st : tc.tryBody)
                collectUnsupported(*st);
            for (const auto& c : tc.catchClauses)
                for (const auto& st : c.body)
                    collectUnsupported(*st);
            for (const auto& st : tc.finallyBody)
                collectUnsupported(*st);
            break;
        }
        case Stmt::Kind::Break:
        case Stmt::Kind::Continue: break;
        case Stmt::Kind::Switch: {
            const auto& sw = static_cast<const SwitchStmt&>(s);
            if (sw.subject) collectUnsupportedExpr(*sw.subject);
            for (const auto& sc : sw.cases) {
                if (sc.value) collectUnsupportedExpr(*sc.value);
                for (const auto& st : sc.body)
                    collectUnsupported(*st);
            }
            break;
        }
        }
    };

    for (const auto& s : fn.body) {
        collectUnsupported(*s);
    }

    // Declaration-site generic type parameters. Only CXCursor_FunctionTemplate
    // carries them; a non-template function leaves templateParams empty so its
    // emitted output stays byte-identical to the pre-generics path.
    if (clang_getCursorKind(funcCursor) == CXCursor_FunctionTemplate) {
        std::vector<std::string> notes;
        collectTemplateParams(ctx.tu, funcCursor, fn.templateParams, notes);
        if (!notes.empty()) {
            for (auto& n : notes) fn.unsupported.push_back(std::move(n));
            fn.fidelity = Fidelity::Inferred;
        }
    }

    return fn;
}

// ===================================================================
// Type extraction: convert a class/struct definition to TranspileType
//
// Populates qualifiedName, non-static data-member fields (straightforward
// FieldDecl members; bit-fields, static members, anonymous unions, and
// member templates are intentionally skipped — they are not part of the
// transpile struct surface), and the inheritance hierarchy from
// CXCursor_CXXBaseSpecifier children. C++ has no class/interface
// distinction, so every base is BaseClassKind::Class (kept parallel to
// baseClasses so cross-language consumers see a uniform shape).
// ===================================================================

struct TypeChildData {
    TranspileType* type;
};

static CXChildVisitResult typeChildVisitor(CXCursor cursor, CXCursor /*parent*/, CXClientData clientData) {
    auto* data = static_cast<TypeChildData*>(clientData);
    CXCursorKind kind = clang_getCursorKind(cursor);

    if (kind == CXCursor_CXXBaseSpecifier) {
        CXType baseTy = clang_getCursorType(cursor);
        TypeNode bn = extractType(baseTy);
        data->type->baseClasses.push_back(std::move(bn));
        data->type->baseClassKinds.push_back(BaseClassKind::Class);
        return CXChildVisit_Continue;
    }

    if (kind == CXCursor_FieldDecl) {
        // Skip bit-fields: their layout is not representable as a plain
        // TranspileField, and emitting one would be lossy/misleading.
        if (clang_Cursor_isBitField(cursor)) {
            return CXChildVisit_Continue;
        }
        TranspileField f;
        f.type = extractType(clang_getCursorType(cursor));
        f.name = cxStr(clang_getCursorSpelling(cursor));
        f.fidelity = Fidelity::Source;
        data->type->fields.push_back(std::move(f));
        return CXChildVisit_Continue;
    }

    return CXChildVisit_Continue;
}

struct TypeTraversalData {
    std::vector<TranspileType>* results;
    std::unordered_set<std::string>* seen;
};

// Returns true for declarations that originate inside a system header (libc++,
// SDK frameworks, clang builtins). Once -isysroot / -resource-dir are wired up,
// the TU pulls in hundreds of stdlib types; without this filter every libc++
// internal (`std::__1::enable_if`, `is_array`, ...) would leak into the
// extracted module.
static bool cursorInSystemHeader(CXCursor cursor) {
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    return clang_Location_isInSystemHeader(loc) != 0;
}

// (formerly `typeVisitor` — folded into `unifiedVisitor` below)

// ===================================================================
// AST traversal: find requested function definitions
// ===================================================================

struct TraversalData {
    const ExtractContext* ctx;
    const std::unordered_set<std::string>* requestedFunctions;
    std::vector<TranspileFunction>* results;
    std::unordered_set<std::string>* found;
};

// (formerly `functionVisitor` — folded into `unifiedVisitor` below)

// ===================================================================
// Unified single-pass dispatcher
// ===================================================================
//
// Carries both visitors' state and fans out per cursor. Replaces two
// back-to-back full TU walks (one for functions, one for types) with a
// single walk that handles both — halving the AST traversal cost per
// translation unit. The per-cursor decisions are identical to what the
// original `functionVisitor` / `typeVisitor` made; the dispatcher just
// runs both fans on the same cursor before deciding whether to recurse.
struct UnifiedTraversalData {
    TraversalData* funcData;
    TypeTraversalData* typeData;
};

static CXChildVisitResult unifiedVisitor(CXCursor cursor, CXCursor /*parent*/, CXClientData clientData) {
    auto* data = static_cast<UnifiedTraversalData*>(clientData);
    CXCursorKind kind = clang_getCursorKind(cursor);

    // --- function fan ---
    // Capture function definitions for the requested set. The original
    // `functionVisitor` recurses into namespaces / class declarations
    // whenever the current cursor is not a function definition; we
    // fold that into the recurse decision at the end of this fn.
    bool isFuncDef = (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod ||
                      kind == CXCursor_Constructor || kind == CXCursor_Destructor ||
                      kind == CXCursor_FunctionTemplate);
    if (isFuncDef && clang_isCursorDefinition(cursor)) {
        std::string qualName = getQualifiedName(cursor);
        if (data->funcData->requestedFunctions->count(qualName) &&
            !data->funcData->found->count(qualName)) {
            data->funcData->found->insert(qualName);
            TranspileFunction fn = convertFunction(*data->funcData->ctx, cursor, qualName);
            data->funcData->results->push_back(std::move(fn));
        }
    }

    // --- type fan ---
    // Capture record / class-template type defs (with the same
    // system-header filter the standalone typeVisitor used).
    bool isRecord = (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl ||
                     kind == CXCursor_ClassTemplate);
    if (isRecord && clang_isCursorDefinition(cursor) && !cursorInSystemHeader(cursor)) {
        std::string qn = getQualifiedName(cursor);
        // De-dup: a class may be visited once per TU; first complete
        // definition wins.
        if (data->typeData->seen->insert(qn).second) {
            TranspileType t;
            t.qualifiedName = qn;
            t.fidelity = Fidelity::Source;
            if (kind == CXCursor_ClassTemplate) {
                std::vector<std::string> notes;
                collectTemplateParams(clang_Cursor_getTranslationUnit(cursor),
                                      cursor, t.templateParams, notes);
                if (!notes.empty()) {
                    t.fidelity = Fidelity::Inferred;
                }
            }
            TypeChildData childData;
            childData.type = &t;
            clang_visitChildren(cursor, typeChildVisitor, &childData);
            data->typeData->results->push_back(std::move(t));
        }
    }

    // --- recurse decision ---
    // Either fan would have asked us to recurse into namespaces /
    // classes / class templates; same target set, so the union is
    // just that set.
    if (kind == CXCursor_Namespace || kind == CXCursor_ClassDecl ||
        kind == CXCursor_StructDecl || kind == CXCursor_ClassTemplate) {
        return CXChildVisit_Recurse;
    }
    return CXChildVisit_Continue;
}

// ===================================================================
// Main entry point
// ===================================================================

static int run() {
    // 1. Read all stdin
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    std::string input = ss.str();

    if (input.empty()) {
        std::cerr << "topo-extract-cpp: empty input on stdin\n";
        return 1;
    }

    // 2. Parse JSON request
    json request;
    try {
        request = json::parse(input);
    } catch (const json::exception& e) {
        std::cerr << "topo-extract-cpp: JSON parse error: " << e.what() << "\n";
        return 1;
    }

    std::vector<std::string> files;
    for (const auto& f : request.at("files")) {
        files.push_back(f.get<std::string>());
    }

    std::unordered_set<std::string> requestedFunctions;
    for (const auto& fn : request.at("functions")) {
        requestedFunctions.insert(fn.get<std::string>());
    }

    // Caller-supplied include paths (optional). Forwarded as -I<path> to
    // libclang so [build].include resolves against the user project. Absent
    // by default; legacy callers continue to send no key.
    std::vector<std::string> userIncludePaths;
    if (auto it = request.find("includePaths"); it != request.end() && it->is_array()) {
        for (const auto& p : *it) {
            if (p.is_string()) userIncludePaths.push_back(p.get<std::string>());
        }
    }

    // Caller-supplied C++ standard (optional). When the project's
    // `[build].standard` is something other than `c++20` the caller is
    // expected to pass it through here so the extractor parses under
    // the same standard as the build, avoiding the silent skew where
    // a C++17 project sees C++20-only diagnostics from topo-check that
    // never appear in the build. The value is the bare standard token
    // (e.g. `"c++17"`, `"c++20"`, `"c++23"`); we prepend `-std=` once.
    std::string cxxStandard = "c++20";
    if (auto it = request.find("cxxStandard"); it != request.end() && it->is_string()) {
        std::string requested = it->get<std::string>();
        if (!requested.empty()) cxxStandard = std::move(requested);
    }

    // symbolTable is available for reference but the extractor primarily
    // relies on libclang AST; we use it to know which functions to look for.
    // (Already captured in requestedFunctions above.)

    // Build clang argv. The argv strings must outlive the parse call;
    // owned in `argStrings`, observed via `argPtrs`.
    // Default `-std=c++20` so libclang can parse concepts
    // (`template <Concept T>`, `requires` clauses) used by the
    // constrained-parameter bound MVP. The caller may downgrade to
    // `c++17` (or upgrade to `c++23`) via the request's `cxxStandard`
    // field; see the protocol comment block at the top of this file.
    std::vector<std::string> argStrings = {
        "-std=" + cxxStandard,
        "-xc++",
    };

    // Resolve clang's resource dir (lib/clang/<major>) from the BYO LLVM
    // toolchain at runtime so a relocated binary finds the builtin headers
    // (<stddef.h>, intrinsics) wherever the user's LLVM lives. The baked
    // compile-time path is only a dev/build-tree fallback.
    {
        std::string rd = topo::platform::llvmResourceDir();
#ifdef TOPO_EXTRACT_CPP_RESOURCE_DIR
        if (rd.empty()) rd = TOPO_EXTRACT_CPP_RESOURCE_DIR;
#endif
        if (!rd.empty() && std::filesystem::exists(rd)) {
            argStrings.push_back("-resource-dir");
            argStrings.push_back(rd);
        }
    }

    // macOS: resolve the active SDK via xcrun so libc++ headers (<memory>,
    // <vector>, ...) can be found. On Linux/Windows the host toolchain's
    // default search paths plus caller-supplied -I cover the standard library;
    // no equivalent runtime discovery is attempted here.
#ifdef __APPLE__
    {
        auto sdkResult = topo::platform::runProcessCapture(
            "xcrun", {"--show-sdk-path"});
        if (sdkResult.exitCode == 0) {
            std::string sdk = sdkResult.stdoutOutput;
            while (!sdk.empty() && (sdk.back() == '\n' || sdk.back() == '\r' || sdk.back() == ' ')) {
                sdk.pop_back();
            }
            if (!sdk.empty() && std::filesystem::exists(sdk)) {
                argStrings.push_back("-isysroot");
                argStrings.push_back(sdk);
                argStrings.push_back("-stdlib=libc++");
            }
        }
    }
#endif

    for (const auto& p : userIncludePaths) {
        argStrings.push_back("-I" + p);
    }

    std::vector<const char*> argPtrs;
    argPtrs.reserve(argStrings.size());
    for (const auto& s : argStrings) argPtrs.push_back(s.c_str());

    // 3. Parse each file and extract functions
    CXIndex index = clang_createIndex(
        /*excludeDeclarationsFromPCH=*/0,
        /*displayDiagnostics=*/0);

    TranspileModule module;
    std::unordered_set<std::string> found;
    std::unordered_set<std::string> seenTypes;
    std::vector<std::string> diagnostics;
    bool sawFileNotFound = false;

    for (const auto& file : files) {
        // We need function bodies (avoid CXTranslationUnit_SkipFunctionBodies).
        CXTranslationUnit tu =
            clang_parseTranslationUnit(index, file.c_str(),
                                       argPtrs.empty() ? nullptr : argPtrs.data(),
                                       static_cast<int>(argPtrs.size()),
                                       nullptr, 0, CXTranslationUnit_None);

        if (!tu) {
            diagnostics.push_back("failed to parse translation unit: " + file);
            continue;
        }

        // Check for parse errors. "file not found" specifically means an
        // #include could not be resolved; if we proceed, clang's error-recovery
        // collapses every unresolved template type to `int`, which silently
        // corrupts the lifted TranspileModel. Treat that as a fatal extractor
        // error so the caller (TranspileDriver) surfaces a real failure
        // rather than emitting a structurally-valid but semantically wrong
        // module.
        unsigned numDiags = clang_getNumDiagnostics(tu);
        for (unsigned i = 0; i < numDiags; ++i) {
            CXDiagnostic diag = clang_getDiagnostic(tu, i);
            CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);
            if (sev >= CXDiagnostic_Error) {
                std::string msg = cxStr(clang_getDiagnosticSpelling(diag));
                diagnostics.push_back("parse error in " + file + ": " + msg);
                if (msg.find("file not found") != std::string::npos) {
                    sawFileNotFound = true;
                }
            }
            clang_disposeDiagnostic(diag);
        }

        ExtractContext ctx;
        ctx.tu = tu;

        CXCursor rootCursor = clang_getTranslationUnitCursor(tu);

        TraversalData data;
        data.ctx = &ctx;
        data.requestedFunctions = &requestedFunctions;
        data.results = &module.functions;
        data.found = &found;

        TypeTraversalData typeData;
        typeData.results = &module.types;
        typeData.seen = &seenTypes;

        // Single AST walk: dispatches to both fans per cursor. Each fan's
        // decision is identical to the standalone visitor it replaces; the
        // saving is the cursor-tree traversal cost itself, which scales with
        // the size of the TU's included header set.
        UnifiedTraversalData unified;
        unified.funcData = &data;
        unified.typeData = &typeData;
        clang_visitChildren(rootCursor, unifiedVisitor, &unified);

        clang_disposeTranslationUnit(tu);
    }

    clang_disposeIndex(index);

    // Report functions that were requested but not found
    for (const auto& fn : requestedFunctions) {
        if (!found.count(fn)) {
            diagnostics.push_back("function not found: " + fn);
        }
    }

    // Log diagnostics to stderr (they are informational; stdout is for JSON)
    for (const auto& d : diagnostics) {
        std::cerr << "topo-extract-cpp: " << d << "\n";
    }

    // Refuse to emit a corrupt module when an #include failed to resolve.
    // The caller treats non-zero exit as extraction failure; better to fail
    // loud than to silently substitute `int` for every unresolved template.
    if (sawFileNotFound) {
        std::cerr << "topo-extract-cpp: aborting: one or more #include directives "
                     "could not be resolved; pass includePaths in the request "
                     "or ensure the host SDK is reachable.\n";
        return 1;
    }

    // 4. Serialize and write to stdout
    json output = serializeModule(module);
    std::cout << output.dump() << std::flush;

    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        std::cerr << "topo-extract-cpp: fatal error: " << e.what() << "\n";
        return 1;
    }
}
