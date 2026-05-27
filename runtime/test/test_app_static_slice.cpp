// End-to-end acceptance for the topo-app C++ compile-time static-analysis
// front-end.
//
// Two front-ends produce the SAME .topo from C++ source without compiling
// or executing it:
//
//   * the clang-AST front-end `topo-app-static-cpp` — the PRIMARY path,
//     post-preprocessor and fully type-resolved
//     (topo-lang-cpp/topo-build/static-frontend/);
//   * the regex MVP `ta::stat::parse_app_source` — the documented
//     FALLBACK for when `clang` is not resolvable, kept header-only at
//     runtime/include/topo/app_static.h.
//
// This test pins both. T1..T5 are the vertical-slice parity cases: the
// clang-AST front-end must reproduce the regex MVP's result and stay
// semantically equivalent to the runtime registration bridge. R1..R5 are
// the robustness cases — one per regex-MVP limitation the clang-AST
// upgrade exists to eliminate; each MUST pass on the clang-AST front-end
// and is shown to fail on the regex MVP.
//
// Self-contained: no GTest, no LLVM link. Build with the system compiler;
// run with the FRESH toolchain binaries via the three env vars:
//
//   TOPO_BIN              fresh build/.../topo
//   TOPO_CHECK_BIN        fresh build/.../topo-check
//   TOPO_APP_STATIC_CPP   fresh build/.../topo-app-static-cpp
//
// Every check is a hard assertion. No skips: a missing binary fails
// loudly (CLAUDE.md — a skip is not a pass).

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <topo/Platform/TempFile.h>
#include <topo/app.h>          // runtime bridge — the equivalence oracle
#include <topo/app_check.h>
#include <topo/app_config.h>
#include <topo/app_emit.h>
#include <topo/app_readback.h>
#include <topo/app_static.h>   // the regex MVP fallback (parity oracle)

namespace ta = topo::app;

static int g_failures = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "FAIL [" << __LINE__ << "] " << msg << "\n";      \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

// --- driving the clang-AST front-end binary ------------------------------

// Resolve the freshly-built topo-app-static-cpp executable. An explicit
// env var wins (CI / the CTest wrapper sets it to the fresh tree);
// otherwise a clear, loud error — silently degrading defeats the test.
static std::string static_cpp_bin() {
    if (const char* p = std::getenv("TOPO_APP_STATIC_CPP")) return p;
    throw std::runtime_error(
        "TOPO_APP_STATIC_CPP unset: point it at the FRESH "
        "build/.../topo-app-static-cpp binary");
}

// The topo-app runtime header include dir, so the analyzed C++ source's
// `#include <topo/app.h>` resolves. Wired by CMake at build time; an env
// var override is also honoured.
static std::string runtime_include_dir() {
    if (const char* p = std::getenv("TOPO_APP_RUNTIME_INCLUDE")) return p;
#ifdef TOPO_APP_RUNTIME_INCLUDE_DIR
    return TOPO_APP_RUNTIME_INCLUDE_DIR;
#else
    throw std::runtime_error(
        "TOPO_APP_RUNTIME_INCLUDE unset and TOPO_APP_RUNTIME_INCLUDE_DIR "
        "not compiled in: cannot resolve topo/app.h for the analyzed "
        "source");
#endif
}

// Argv for running the front-end against a temp source file, with
// ``-- -I<runtime-include>`` appended so the analyzed source's
// ``#include <topo/app.h>`` resolves. Returns the argv vector directly
// — argv-style exec via topo::platform::runProcessCapture replaced the
// previous shell-string concatenation (audit issue
// topo-lang-cpp-app-readback-shell-popen).
static std::vector<std::string> static_args(const std::string& src) {
    return {src, "--", "-I" + runtime_include_dir()};
}

// Generate a per-process unique scratch file path under the platform
// temp directory. ``ta::temp_path`` was removed when its predictable-
// name TOCTOU surface was retired; this local helper keeps the test's
// existing "write source → run analyser → delete source" flow without
// dragging the predictable-name footgun back in.
static std::string make_scratch_path(const std::string& name) {
    static std::atomic<unsigned long> g_counter{0};
    unsigned long nonce = g_counter.fetch_add(1, std::memory_order_relaxed);
    auto p = topo::platform::tempDirectory() /
             ("topo_app_test-" + std::to_string(nonce) + "-" + name);
    return p.string();
}

// Write C++ source to a temp file, run the clang-AST front-end against
// it, and return the emitted .topo. Throws on a non-zero exit so an
// analysis failure surfaces loudly rather than as an empty graph.
//
// `#include <topo/app.h>` is prepended when absent: the clang-AST path
// needs the topo-app surface types (ta::App / Record / Field / parallel)
// fully declared so they resolve in the AST. The regex MVP fallback does
// not — it scans text — so the robustness cases keep their raw source
// for the regex-misread assertions.
static std::string emit_via_clang_ast(const std::string& cpp_source,
                                      const char* tag) {
    std::string body = cpp_source;
    if (body.find("topo/app.h") == std::string::npos)
        body = "#include <topo/app.h>\n" + body;

    std::string src = make_scratch_path(std::string(tag) + ".cpp");
    {
        std::ofstream f(src);
        f << body;
    }
    ta::ProcResult r = ta::run_capture(static_cpp_bin(), static_args(src));
    std::remove(src.c_str());
    if (r.exit_code != 0)
        throw std::runtime_error(
            std::string("topo-app-static-cpp failed for ") + tag + ":\n" +
            r.stdout_text);
    return r.stdout_text;
}

// The clang-AST front-end's Graph: emit .topo, read it back through the
// real `topo` parser. read_topo() going through the actual parser is
// itself the grammar-conformance proof.
static ta::Graph graph_via_clang_ast(const std::string& cpp_source,
                                     const char* tag) {
    return ta::read_topo(emit_via_clang_ast(cpp_source, tag));
}

// --- the canonical vertical-slice program --------------------------------
//
// Ordinary C++ source using the runtime registration API. Never compiled
// or run by the test; the front-ends read it as data. Same handlers /
// In-Out / flow as the runtime-bridge test's build_app.
static const char* kSliceSource = R"CPP(
#include <string>
#include <topo/app.h>
namespace ta = topo::app;

using OrderRec =
    ta::Record<ta::Field<"id", int>, ta::Field<"amount", double>>;

static OrderRec parse(std::string raw) { (void)raw; return {}; }
static OrderRec validate(OrderRec o) { return o; }
static bool persist(OrderRec o) { (void)o; return true; }

ta::App build() {
    ta::App app("orders");
    app.handler(&parse, "parse", "parse a raw order line");
    app.handler(&validate, "validate", "validate the parsed order");
    app.handler(&persist, "persist", "persist the validated order");
    app.flow("order_pipeline", "parse", "validate", "persist");
    return app;
}
)CPP";

// The runtime-built equivalence oracle: the SAME program, Graph from
// app.h executing the registration calls (function_traits).
using OrderRec =
    ta::Record<ta::Field<"id", int>, ta::Field<"amount", double>>;
static OrderRec parse_fn(std::string raw) { (void)raw; return {}; }
static OrderRec validate_fn(OrderRec o) { return o; }
static bool persist_fn(OrderRec o) { (void)o; return true; }

static ta::App build_runtime(const std::string& ns = "orders") {
    ta::App app(ns);
    app.handler(&parse_fn, "parse", "parse a raw order line");
    app.handler(&validate_fn, "validate", "validate the parsed order");
    app.handler(&persist_fn, "persist", "persist the validated order");
    app.flow("order_pipeline", "parse", "validate", "persist");
    return app;
}

// Wrap a Graph (from the clang-AST front-end's readback) into an App so
// the reused app_check.h::check() — which only reads app.graph() — runs
// against it unchanged.
static ta::App app_of(const ta::Graph& g) {
    ta::App app(g.namespace_name());
    app.graph() = g;
    return app;
}

// ---- T1: the clang-AST front-end produces an enumerable graph -----------

static void T1_skeleton() {
    ta::Graph g = graph_via_clang_ast(kSliceSource, "t1");
    CHECK(g.namespace_name() == "orders", "T1 namespace from App ctor");

    CHECK(g.handlers().size() == 3, "T1 handler count");
    CHECK(g.handler("parse") != nullptr, "T1 parse present");
    CHECK(g.handler("validate") != nullptr, "T1 validate present");
    CHECK(g.handler("persist") != nullptr, "T1 persist present");

    // In/Out read statically from the referenced fn signatures.
    CHECK(g.handler("parse")->in_type->topo() == "str",
          "T1 parse In (std::string -> str)");
    CHECK(g.handler("parse")->out_type.topo() ==
              "record<id: int, amount: float>",
          "T1 parse Out (Record alias resolved from the AST)");
    CHECK(g.handler("persist")->out_type.topo() == "bool",
          "T1 persist Out (bool)");

    CHECK(g.has_flow(), "T1 flow present");
    CHECK(g.flow().edges.size() == 3,
          "T1 edges parse->validate->persist->void");

    // A no-parameter handler is a legal source handler.
    static const char* kSrc = R"CPP(
        namespace ta = topo::app;
        int seed() { return 0; }
        ta::App b() { ta::App a("src"); a.handler(&seed, "seed"); return a; }
    )CPP";
    ta::Graph s = graph_via_clang_ast(kSrc, "t1_src");
    CHECK(!s.handler("seed")->in_type.has_value(),
          "T1 source handler has no input");
}

// ---- T2: emitted .topo parses under the fresh grammar -------------------

static void T2_emit() {
    std::string text = emit_via_clang_ast(kSliceSource, "t2");
    CHECK(text.find("handler parse(str in) -> "
                    "record<id: int, amount: float>;") != std::string::npos,
          "T2 emitted parse signature");
    CHECK(text.find("flow order_pipeline {") != std::string::npos,
          "T2 emitted flow header");
    // read_topo throws if `topo` rejects the source.
    ta::Graph g2 = ta::read_topo(text);
    CHECK(g2.namespace_name() == "orders", "T2 readback namespace");
}

// ---- T3: round-trip + static-vs-runtime equivalence ---------------------

static void T3_roundtrip_and_parity() {
    ta::Graph stat = graph_via_clang_ast(kSliceSource, "t3");

    // Round-trip the clang-AST graph through the real parser again.
    ta::Graph g2 = ta::read_topo(ta::emit_topo(stat));
    CHECK(stat.equivalent_to(g2),
          "T3 clang-AST graph survives .topo round-trip");

    // Headline: the clang-AST front-end and the runtime registration
    // bridge produce semantically equivalent graphs for the SAME
    // program, and emit byte-identical .topo.
    ta::App rt_app = build_runtime();
    CHECK(stat.equivalent_to(rt_app.graph()),
          "T3 clang-AST graph == runtime graph (semantic key)");
    CHECK(ta::emit_topo(stat) == ta::emit_topo(rt_app.graph()),
          "T3 clang-AST .topo == runtime .topo (byte-identical)");

    // The regex MVP fallback agrees on the vertical slice — parity of
    // the two static front-ends.
    ta::App regex_app = ta::stat::parse_app_source(kSliceSource);
    CHECK(stat.equivalent_to(regex_app.graph()),
          "T3 clang-AST graph == regex-MVP graph (slice parity)");

    // The .topo is a view: hand-reorder edges, read back, still equal.
    std::string text = ta::emit_topo(stat);
    std::string from = "      parse -> validate;\n      validate -> persist;";
    std::string to = "      validate -> persist;\n      parse -> validate;";
    auto pos = text.find(from);
    CHECK(pos != std::string::npos, "T3 locate edges to hand-edit");
    if (pos != std::string::npos) {
        std::string edited = text;
        edited.replace(pos, from.size(), to);
        CHECK(stat.equivalent_to(ta::read_topo(edited)),
              "T3 hand edit survives readback");
    }
}

// ---- T4: zero hand-written .topo, the existing topo-check runs ----------

static const char* kCompliantSource = R"CPP(
namespace ta = topo::app;
int parse(int raw) { return raw + 1; }
int enrich(int v) { return v * 2; }
int audit(int v) { return v; }
float total(int v) { return (float)v + 0.5f; }
ta::App build() {
    ta::App app("orders");
    app.handler(&parse, "parse");
    app.handler(&enrich, "enrich");
    app.handler(&audit, "audit");
    app.handler(&total, "total");
    app.flow("pipeline", "parse", ta::parallel("enrich", "audit"), "total");
    return app;
}
)CPP";

static const char* kCompliantBodies =
    "int parse(int raw) {\n"
    "    return raw + 1;\n"
    "}\n"
    "int enrich(int v) {\n"
    "    return v * 2;\n"
    "}\n"
    "int audit(int v) {\n"
    "    return v;\n"
    "}\n"
    "float total(int v) {\n"
    "    return (float)v + 0.5f;\n"
    "}\n";

static const char* kViolatingBodies =
    "static int g_log = 0;\n"
    "int parse(int raw) {\n"
    "    return raw + 1;\n"
    "}\n"
    "int enrich(int v) {\n"
    "    return v * 2;\n"
    "}\n"
    "int audit(int v) {\n"
    "    g_log = g_log + v;\n"
    "    return v;\n"
    "}\n"
    "float total(int v) {\n"
    "    return (float)v + 0.5f;\n"
    "}\n";

static std::string write_temp_src(const char* body, const char* name) {
    std::string p = make_scratch_path(name);
    std::ofstream f(p);
    f << body;
    return p;
}

static void T4_zero_declaration_check() {
    // Compliant flow passes zero-decl topo-check, driven entirely off the
    // clang-AST front-end's graph (no hand-written .topo anywhere).
    {
        ta::Graph g = graph_via_clang_ast(kCompliantSource, "t4_ok");
        ta::App app = app_of(g);
        std::string src = write_temp_src(kCompliantBodies, "compliant_s.cpp");
        ta::CheckResult r = ta::check(app, {src});
        std::remove(src.c_str());
        CHECK(r.passed, "T4 compliant app passes zero-decl check\n"
                            << r.output);
    }

    // The violating case: audit() — same-stage parallel candidate with
    // enrich() — writes a file-scope global. Core PurityCheck must flag
    // it even though no .topo was hand-written.
    {
        ta::Graph g = graph_via_clang_ast(kCompliantSource, "t4_bad");
        ta::App app = app_of(g);
        std::string src = write_temp_src(kViolatingBodies, "violating_s.cpp");
        ta::CheckResult r = ta::check(app, {src});
        std::remove(src.c_str());
        CHECK(!r.passed,
              "T4 violating handler must be flagged by zero-decl topo-check "
              "on the clang-AST graph\n"
                  << r.output);
    }
}

// ---- T5: config() snapshot lists the full statically built graph --------

static void T5_config_entry() {
    ta::Graph g = graph_via_clang_ast(kSliceSource, "t5");
    ta::App app = app_of(g);
    ta::Snapshot snap = ta::config(app).snapshot();
    CHECK(snap.namespace_name == "orders", "T5 snapshot namespace");
    CHECK(snap.handlers.size() == 3, "T5 snapshot handler count");
    CHECK(snap.flow.has_value(), "T5 snapshot flow present");
    CHECK(snap.flow->name == "order_pipeline", "T5 snapshot flow name");
    CHECK(snap.flow->edges.size() == 3, "T5 snapshot edge count");

    CHECK(ta::config(app).emit_topo() == ta::emit_topo(app.graph()),
          "T5 config.emit_topo == raw emitter");
}

// =========================================================================
// R1..R5 — the five regex-MVP limitations the clang-AST upgrade removes.
//
// Each case is a C++ program written so the regex MVP misreads it; the
// clang-AST front-end must produce the correct graph. The regex MVP's
// failure is asserted alongside so the contrast is explicit and the
// clang-AST front-end's advantage is proven, not assumed.
// =========================================================================

// Helper: did the regex MVP fail to recover a correct graph for `src`?
// "Fail" == it threw, OR it built a graph that is NOT equivalent to the
// expected one. Returns a human note describing what happened.
static bool regex_mvp_misreads(const std::string& src,
                               const ta::Graph& expected,
                               std::string& note) {
    try {
        ta::App a = ta::stat::parse_app_source(src);
        if (a.graph().equivalent_to(expected)) {
            note = "regex MVP unexpectedly produced an equivalent graph";
            return false;
        }
        note = "regex MVP produced a DIFFERENT (wrong) graph";
        return true;
    } catch (const std::exception& e) {
        note = std::string("regex MVP threw: ") + e.what();
        return true;
    }
}

// ---- R1: macro-expanded registration ------------------------------------
//
// The registration calls are hidden behind a macro. The regex scanner
// only sees the macro *token* `REGISTER(parse)` — never the expanded
// `.handler(&parse, "parse")` — so it finds zero handlers. clang dumps
// the post-preprocessor AST: the expanded CXXMemberCallExpr is right
// there.
static void R1_macro_expanded_registration() {
    static const char* kSrc = R"CPP(
        #include <string>
        namespace ta = topo::app;
        int parse(int raw) { return raw; }
        int finalize(int v) { return v; }
        #define REGISTER(app, fn) app.handler(&fn, #fn)
        #define WIRE(app, a, b) app.flow("pipe", #a, #b)
        ta::App build() {
            ta::App app("macroed");
            REGISTER(app, parse);
            REGISTER(app, finalize);
            WIRE(app, parse, finalize);
            return app;
        }
    )CPP";
    ta::Graph g = graph_via_clang_ast(kSrc, "r1");
    CHECK(g.namespace_name() == "macroed", "R1 namespace through macro");
    CHECK(g.handlers().size() == 2, "R1 both macro-registered handlers seen");
    CHECK(g.handler("parse") && g.handler("finalize"),
          "R1 macro-expanded handler names recovered");
    CHECK(g.has_flow() && g.flow().edges.size() == 2,
          "R1 macro-expanded flow recovered");

    std::string note;
    CHECK(regex_mvp_misreads(kSrc, g, note),
          "R1 regex MVP must misread macro-expanded registration; " << note);
}

// ---- R2: auto-return handlers -------------------------------------------
//
// A handler declared `auto fn(...)` has no syntactic return type for the
// regex scanner to capture — its function regex needs an explicit return
// token, so an `auto` handler is missed or its Out is unrecoverable.
// clang deduces the real return type and reports it in the AST.
static void R2_auto_return_handler() {
    static const char* kSrc = R"CPP(
        #include <string>
        namespace ta = topo::app;
        auto load(std::string raw) { (void)raw; return 7; }
        auto scale(int v) { return v * 2.0; }
        decltype(auto) tag(double d) { return d > 0.0; }
        ta::App build() {
            ta::App app("autos");
            app.handler(&load, "load");
            app.handler(&scale, "scale");
            app.handler(&tag, "tag");
            app.flow("p", "load", "scale", "tag");
            return app;
        }
    )CPP";
    ta::Graph g = graph_via_clang_ast(kSrc, "r2");
    CHECK(g.handlers().size() == 3, "R2 all auto-return handlers seen");
    // clang deduced: load -> int, scale -> double, tag -> bool.
    CHECK(g.handler("load") && g.handler("load")->out_type.topo() == "int",
          "R2 auto load Out deduced to int");
    CHECK(g.handler("scale") && g.handler("scale")->out_type.topo() ==
                                    "float",
          "R2 auto scale Out deduced to float (double)");
    CHECK(g.handler("tag") && g.handler("tag")->out_type.topo() == "bool",
          "R2 decltype(auto) tag Out deduced to bool");

    std::string note;
    CHECK(regex_mvp_misreads(kSrc, g, note),
          "R2 regex MVP must misread auto-return handlers; " << note);
}

// ---- R3: cross-line / indirect registration -----------------------------
//
// The registration calls are split across many lines AND the registered
// name arrives as a C++ adjacent-string-literal concatenation
// (`"in" "gest"`). The regex MVP's handler pattern captures only the
// FIRST literal fragment, so it registers the handler under the wrong,
// truncated name — a silent mis-read. clang concatenates adjacent string
// literals into one StringLiteral whose value is the full name, and sees
// one CXXMemberCallExpr regardless of source-line layout.
static void R3_cross_line_registration() {
    static const char* kSrc = R"CPP(
        #include <string>
        namespace ta = topo::app;
        int ingest(int raw) { return raw; }
        int reduce(int v) { return v; }
        ta::App build() {
            ta::App app("crossline");
            app
                .handler(
                    &ingest
                    ,
                    "in" "gest"
                );
            app.handler(
                &reduce,
                "re"
                "duce"
            );
            app.flow(
                "p"
                ,
                "ingest"
                ,
                "reduce"
            );
            return app;
        }
    )CPP";
    ta::Graph g = graph_via_clang_ast(kSrc, "r3");
    CHECK(g.handlers().size() == 2, "R3 cross-line handlers both seen");
    // The full, concatenated names — not the truncated "in" / "re".
    CHECK(g.handler("ingest") && g.handler("reduce"),
          "R3 cross-line + adjacent-literal handler names recovered whole");
    CHECK(g.handler("in") == nullptr && g.handler("re") == nullptr,
          "R3 names are not truncated to the first literal fragment");
    CHECK(g.has_flow() && g.flow().name == "p" && g.flow().edges.size() == 2,
          "R3 cross-line flow recovered");

    std::string note;
    CHECK(regex_mvp_misreads(kSrc, g, note),
          "R3 regex MVP must misread cross-line/adjacent-literal "
          "registration; " << note);
}

// ---- R4: record field types through using / typedef / template alias ----
//
// A record field's type arrives through an alias chain
// (`using Money = double;`, `typedef long Id;`, a templated alias). The
// regex MVP resolves a record field's type by spelling lookup against
// the aliases it scanned, but a chain or a template alias defeats the
// flat lookup. clang's canonical qualType desugars the whole chain.
static void R4_alias_chain_record_fields() {
    static const char* kSrc = R"CPP(
        #include <string>
        namespace ta = topo::app;
        typedef long RawId;
        using Id = RawId;
        using Money = double;
        template <class T> using Boxed = T;
        using OrderRec = ta::Record<
            ta::Field<"id", Id>,
            ta::Field<"amount", Money>,
            ta::Field<"verified", Boxed<bool>>>;
        OrderRec parse(std::string raw) { (void)raw; return {}; }
        bool persist(OrderRec o) { (void)o; return true; }
        ta::App build() {
            ta::App app("aliased");
            app.handler(&parse, "parse");
            app.handler(&persist, "persist");
            app.flow("p", "parse", "persist");
            return app;
        }
    )CPP";
    ta::Graph g = graph_via_clang_ast(kSrc, "r4");
    CHECK(g.handlers().size() == 2, "R4 handler count");
    // Id -> long -> int band; Money -> double -> float band; Boxed<bool>
    // -> bool. The record field types resolve through the chains.
    CHECK(g.handler("parse") &&
              g.handler("parse")->out_type.topo() ==
                  "record<id: int, amount: float, verified: bool>",
          "R4 record field types resolved through alias chains");

    std::string note;
    CHECK(regex_mvp_misreads(kSrc, g, note),
          "R4 regex MVP must misread alias-chain record fields; " << note);
}

// ---- R5: nested / aliased-namespace handler resolution ------------------
//
// Two functions named `parse` live in different namespaces with
// DIFFERENT signatures; the real handler is the nested-namespace one,
// referenced through a namespace alias. The regex MVP keys functions on
// a flat last-component bare name — the two `parse` definitions collide
// in its flat table and the last scanned one wins, so it resolves the
// registration to the WRONG `parse` (or throws on its 2-arg shape).
// clang resolves `&outer::inner::parse` to the exact decl by its unique
// id; the AST walk descends every NamespaceDecl, so the right function
// is found regardless of namespace nesting or aliasing.
static void R5_nested_namespace_handlers() {
    static const char* kSrc = R"CPP(
        #include <string>
        namespace ta = topo::app;
        namespace outer { namespace inner {
            // The REAL handler: one input, scalar In/Out.
            int parse(int raw) { return raw; }
            int verify(int v) { return v; }
        }}
        namespace alias_ns = outer::inner;
        // A DECOY parse at file scope with an incompatible 2-input
        // signature — a handler must have at most one input. It is
        // declared AFTER the real one, so the regex MVP's flat
        // last-wins function table binds the registered `parse` to this
        // decoy and mis-reads (or rejects) it.
        int parse(int a, int b) { return a + b; }
        ta::App build() {
            ta::App app("nested");
            app.handler(&outer::inner::parse, "parse");
            app.handler(&alias_ns::verify, "verify");
            app.flow("p", "parse", "verify");
            return app;
        }
    )CPP";
    ta::Graph g = graph_via_clang_ast(kSrc, "r5");
    CHECK(g.namespace_name() == "nested", "R5 namespace");
    CHECK(g.handlers().size() == 2, "R5 nested-namespace handlers seen");
    CHECK(g.handler("parse") && g.handler("verify"),
          "R5 nested + aliased-namespace handler symbols resolved");
    // The clang-AST front-end bound to the one-input nested `parse`, not
    // the two-input file-scope decoy.
    CHECK(g.handler("parse")->in_type &&
              g.handler("parse")->in_type->topo() == "int",
          "R5 resolved the exact nested parse (one input), not the decoy");
    CHECK(g.has_flow() && g.flow().edges.size() == 2, "R5 flow recovered");

    std::string note;
    CHECK(regex_mvp_misreads(kSrc, g, note),
          "R5 regex MVP must misread namespace-collided handler names; "
              << note);
}

int main() {
    if (!std::getenv("TOPO_BIN") || !std::getenv("TOPO_CHECK_BIN") ||
        !std::getenv("TOPO_APP_STATIC_CPP")) {
        std::cerr << "ERROR: set TOPO_BIN, TOPO_CHECK_BIN and "
                     "TOPO_APP_STATIC_CPP to the FRESH toolchain binaries\n";
        return 2;
    }
    try {
        T1_skeleton();
        T2_emit();
        T3_roundtrip_and_parity();
        T4_zero_declaration_check();
        T5_config_entry();
        R1_macro_expanded_registration();
        R2_auto_return_handler();
        R3_cross_line_registration();
        R4_alias_chain_record_fields();
        R5_nested_namespace_handlers();
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        return 1;
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS: T1..T5 vertical-slice parity + R1..R5 "
                     "robustness — the clang-AST front-end reproduces the "
                     "regex MVP on the slice and removes all five of its "
                     "documented limitations\n";
        return 0;
    }
    std::cout << g_failures << " FAILURE(S)\n";
    return 1;
}
