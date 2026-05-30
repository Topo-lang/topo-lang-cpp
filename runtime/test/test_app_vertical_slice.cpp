// End-to-end acceptance for the topo-app C++ vertical slice — the C++
// projection of topo-lang-python/runtime/test/test_vertical_slice.py
// T1..T5. Self-contained: no project CMake, no GTest, no LLVM. Compile
// with the system compiler and run with the FRESH toolchain binaries on
// PATH via TOPO_BIN / TOPO_CHECK_BIN.
//
//   clang++ -std=c++20 -Wall -Wextra \
//     -I topo-lang-cpp/runtime/include \
//     topo-lang-cpp/runtime/test/test_app_vertical_slice.cpp -o /tmp/tav
//   TOPO_BIN=<fresh>/topo TOPO_CHECK_BIN=<fresh>/topo-check /tmp/tav
//
// Every check is a hard assertion; a failure aborts with a located
// message. No skips: the round-trip / check cases require the fresh
// binaries by design and fail loudly if they are absent.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <topo/Platform/TempFile.h>
#include <topo/app.h>
#include <topo/app_check.h>
#include <topo/app_config.h>
#include <topo/app_emit.h>
#include <topo/app_readback.h>

namespace ta = topo::app;

static int g_failures = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "FAIL [" << __LINE__ << "] " << msg << "\n";      \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

// The shared OrderRec schema — the C++ analogue of the Python reference's
// OrderRec = topo.Record[("id", int), ("amount", float)]. Field names are
// explicit compile-time metadata on the type (no reflection involved).
using OrderRec =
    ta::Record<ta::Field<"id", int>, ta::Field<"amount", double>>;

// Plain handler callables. They take/return ordinary C++ values and the
// Record schema type; In/Out are read from these signatures by
// function_traits, never re-declared.
static OrderRec parse(std::string raw) {
    (void)raw;
    return {};
}
static OrderRec validate(OrderRec o) { return o; }
static bool persist(OrderRec o) {
    (void)o;
    return true;
}

static ta::App build_app(const std::string& ns = "orders") {
    ta::App app(ns);
    app.handler(&parse, "parse", "parse a raw order line");
    app.handler(&validate, "validate", "validate the parsed order");
    app.handler(&persist, "persist", "persist the validated order");
    app.flow("order_pipeline", "parse", "validate", "persist");
    return app;
}

// ---- T1: registration produces an enumerable graph ----------------------

static int seed_handler() { return 0; }
static int double_handler(int n) { return n * 2; }

static void T1_skeleton() {
    ta::App app = build_app();
    const ta::Graph& g = app.graph();
    CHECK(g.namespace_name() == "orders", "T1 namespace");

    CHECK(g.handlers().size() == 3, "T1 handler count");
    CHECK(g.handlers()[0].name == "parse", "T1 h0");
    CHECK(g.handlers()[1].name == "validate", "T1 h1");
    CHECK(g.handlers()[2].name == "persist", "T1 h2");

    CHECK(g.handler("parse")->in_type->topo() == "str", "T1 parse In");
    CHECK(g.handler("parse")->out_type.topo() ==
              "record<id: int, amount: float>",
          "T1 parse Out");
    CHECK(g.handler("persist")->out_type.topo() == "bool", "T1 persist Out");

    CHECK(g.has_flow(), "T1 flow present");
    CHECK(g.flow().edges.size() == 3,
          "T1 edges parse->validate->persist->void");

    // A no-parameter handler is a legal source handler.
    ta::App src("src");
    src.handler(&seed_handler, "seed");
    CHECK(!src.graph().handler("seed")->in_type.has_value(),
          "T1 source handler has no input");

    // A handler stays a plain fn after registration: still directly
    // callable with zero framework bootstrap.
    ta::App x("x");
    auto d = x.handler(&double_handler, "double");
    CHECK(d(21) == 42, "T1 handler stays independently callable");
}

// ---- T2: emitted .topo parses under the merged grammar ------------------

static void T2_emit() {
    ta::App app = build_app();
    std::string text = ta::config(app).emit_topo();
    CHECK(text.find("handler parse(str in) -> "
                    "record<id: int, amount: float>;") != std::string::npos,
          "T2 emitted parse signature");
    CHECK(text.find("flow order_pipeline {") != std::string::npos,
          "T2 emitted flow header");
    // read_topo throws if `topo` rejects the source — parsing it is
    // itself the grammar-conformance proof.
    ta::Graph g2 = ta::read_topo(text);
    CHECK(g2.namespace_name() == "orders", "T2 readback namespace");
}

// ---- T3: graph -> .topo -> graph' with graph == graph' ------------------

static void T3_roundtrip() {
    ta::App app = build_app();
    ta::Graph g2 = ta::config(app).roundtrip();
    CHECK(app.graph().equivalent_to(g2),
          "T3 semantic equivalence after round-trip");

    // The .topo is a view, not an opaque IR: reorder edges by hand, read
    // back, still semantically equivalent.
    std::string text = ta::config(app).emit_topo();
    std::string from = "      parse -> validate;\n      validate -> persist;";
    std::string to = "      validate -> persist;\n      parse -> validate;";
    auto pos = text.find(from);
    CHECK(pos != std::string::npos, "T3 locate edges to hand-edit");
    std::string edited = text;
    edited.replace(pos, from.size(), to);
    CHECK(app.graph().equivalent_to(ta::read_topo(edited)),
          "T3 hand edit survives readback");
}

// ---- T4: zero hand-written .topo, the existing topo-check runs ----------

// The C++ source given to topo-check. The regex-based
// CppSymbolAccessExtractor (topo-lang-cpp/topo-check/.../
// CppSymbolAccessExtractor.cpp) detects a global write only when it is a
// statement on its own line; functions are file-scope so the extractor's
// bare names match the flow's stage members. This is the same formatting
// the canonical purity_cpp_fail_01 fixture uses — written this way for
// the extractor, not for style.
static const char* kCompliantSrc =
    "// compliant: every flow handler is pure.\n"
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

// The violating variant: audit() — a same-stage parallel candidate with
// enrich() — writes a file-scope global. Core PurityCheck must flag it
// even though no .topo was written by hand.
static const char* kViolatingSrc =
    "// violating: audit() writes a file-scope global as a parallel "
    "candidate.\n"
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

static int h_parse(int raw) { return raw + 1; }
static int h_enrich(int v) { return v * 2; }
static int h_audit(int v) { return v; }
static float h_total(int v) { return (float)v + 0.5f; }

static ta::App build_parallel_app() {
    ta::App app("orders");
    app.handler(&h_parse, "parse");
    app.handler(&h_enrich, "enrich");
    app.handler(&h_audit, "audit");
    app.handler(&h_total, "total");
    app.flow("pipeline", "parse", ta::parallel("enrich", "audit"), "total");
    return app;
}

static std::string write_temp_src(const char* body, const char* name) {
    // Unique per-process scratch path; ta::temp_path was retired when
    // subprocesses moved off shell-string popen to argv-style exec.
    static std::atomic<unsigned long> g_counter{0};
    unsigned long nonce = g_counter.fetch_add(1, std::memory_order_relaxed);
    auto pth = topo::platform::tempDirectory() /
               ("topo_app_vslice-" + std::to_string(nonce) + "-" + name);
    std::string p = pth.string();
    std::ofstream f(p);
    f << body;
    return p;
}

static void T4_zero_declaration_check() {
    {
        ta::App app = build_parallel_app();
        std::string src = write_temp_src(kCompliantSrc, "compliant.cpp");
        ta::CheckResult r = ta::check(app, {src});
        std::remove(src.c_str());
        CHECK(r.passed, "T4 compliant app passes\n" << r.output);
    }
    {
        ta::App app = build_parallel_app();
        std::string src = write_temp_src(kViolatingSrc, "violating.cpp");
        ta::CheckResult r = ta::check(app, {src});
        std::remove(src.c_str());
        CHECK(!r.passed,
              "T4 violating handler must be flagged by topo-check\n"
                  << r.output);
    }
}

// ---- T5: config() snapshot lists full graph; emit == emitter ------------

static void T5_config_entry() {
    ta::App app = build_app();
    ta::Snapshot snap = ta::config(app).snapshot();
    CHECK(snap.namespace_name == "orders", "T5 snapshot namespace");
    CHECK(snap.handlers.size() == 3, "T5 snapshot handler count");
    CHECK(snap.flow.has_value(), "T5 snapshot flow present");
    CHECK(snap.flow->name == "order_pipeline", "T5 snapshot flow name");
    CHECK(snap.flow->edges.size() == 3, "T5 snapshot edge count");

    CHECK(ta::config(app).emit_topo() == ta::emit_topo(app.graph()),
          "T5 config.emit_topo == raw emitter");
}

int main() {
    if (!std::getenv("TOPO_BIN") || !std::getenv("TOPO_CHECK_BIN")) {
        std::cerr << "ERROR: set TOPO_BIN and TOPO_CHECK_BIN to the FRESH "
                     "toolchain binaries\n";
        return 2;
    }
    T1_skeleton();
    T2_emit();
    T3_roundtrip();
    T4_zero_declaration_check();
    T5_config_entry();

    if (g_failures == 0) {
        std::cout << "ALL PASS: T1..T5 (15 acceptance points)\n";
        return 0;
    }
    std::cout << g_failures << " FAILURE(S)\n";
    return 1;
}
