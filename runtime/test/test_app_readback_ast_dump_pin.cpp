// Contract test for the ast-dump consumption surface in app_readback.h.
//
// This test pins the producer side (`topo --ast-dump` text output) to
// a byte-for-byte fixture and re-asserts the consumer side
// (`read_topo()` in app_readback.h) still reconstructs the expected
// graph. The fixture sits next to the test as
// `fixtures/ast-dump-roundtrip.topo` (the .topo source) and
// `fixtures/ast-dump-roundtrip.expected.txt` (the canonical dump).
//
// A producer-side drift to the four pinned line forms
// (NamespaceDecl / HandlerDecl / FlowBlock / Edge) fails the
// byte-equality check at this repo's CI, before any downstream
// user-app round-trip silently breaks.
//
// Self-contained: no GTest, header-only, exits non-zero on any
// mismatch. Matches the existing test_app_vertical_slice.cpp style.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <topo/app_model.h>
#include <topo/app_readback.h>

namespace ta = topo::app;

static int g_failures = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::cerr << "FAIL [" << __LINE__ << "] " << msg << "\n"; \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "FATAL: cannot open " << path << "\n";
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string env_or_die(const char* name) {
    const char* v = std::getenv(name);
    if (!v) {
        std::cerr << "FATAL: env var " << name << " unset\n";
        std::exit(2);
    }
    return v;
}

}  // namespace

int main() {
    // Test fixture paths are injected by CMake; see runtime/test/CMakeLists.txt.
    const std::string fixture_topo = env_or_die("AST_DUMP_FIXTURE_TOPO");
    const std::string fixture_expected =
        env_or_die("AST_DUMP_FIXTURE_EXPECTED");

    // --- Producer-side pin: byte-equality on the dumped text -----------
    //
    // The producer (`topo --ast-dump`) is invoked through the same
    // run_capture / topo_bin helpers app_readback.h itself uses, so
    // any environment skew that breaks readback would also break this
    // test (the failures are aligned).
    ta::ProcResult r = ta::run_capture(ta::topo_bin(),
                                       {"--ast-dump", fixture_topo});
    CHECK(r.exit_code == 0,
          "topo --ast-dump rejected the fixture (exit=" << r.exit_code
                                                       << ")\n"
                                                       << r.stdout_text);

    const std::string expected = slurp(fixture_expected);
    if (r.stdout_text != expected) {
        std::cerr << "FAIL: ast-dump output drift detected.\n"
                  << "If the producer (topo-core/lib/AST/ASTPrinter.cpp) "
                     "intentionally changed the dump format, regenerate "
                     "the fixture:\n"
                  << "    build/topo-core/tools/topo/topo --ast-dump \\\n"
                  << "        " << fixture_topo << " \\\n"
                  << "        > " << fixture_expected << "\n"
                  << "and update the four consumer regexes in "
                     "topo-lang-cpp/runtime/include/topo/app_readback.h.\n"
                  << "--- expected ---\n"
                  << expected << "--- actual ---\n"
                  << r.stdout_text << "--- end ---\n";
        ++g_failures;
    }

    // --- Consumer-side pin: parse back into Graph ----------------------
    //
    // Round-trips the very same .topo file the fixture captured, using
    // the same read_topo() the user-facing topo-app uses. Any consumer
    // regex that stops matching the pinned line forms surfaces here.
    const std::string topo_text = slurp(fixture_topo);
    ta::Graph g = ta::read_topo(topo_text);
    CHECK(g.namespace_name() == "orders", "consumer: namespace name");
    CHECK(g.handlers().size() == 3,
          "consumer: handler count expected 3, got " << g.handlers().size());
    CHECK(g.handlers()[0].name == "parse", "consumer: handler[0]");
    CHECK(g.handlers()[1].name == "validate", "consumer: handler[1]");
    CHECK(g.handlers()[2].name == "persist", "consumer: handler[2]");
    CHECK(g.has_flow(), "consumer: flow present");
    if (g.has_flow()) {
        CHECK(g.flow().name == "order_pipeline", "consumer: flow name");
        CHECK(g.flow().edges.size() == 3,
              "consumer: edge count expected 3, got "
                  << g.flow().edges.size());
        if (g.flow().edges.size() == 3) {
            CHECK(g.flow().edges[0].source == "parse" &&
                      g.flow().edges[0].target == "validate",
                  "consumer: edge[0]");
            CHECK(g.flow().edges[1].source == "validate" &&
                      g.flow().edges[1].target == "persist",
                  "consumer: edge[1]");
            CHECK(g.flow().edges[2].source == "persist" &&
                      !g.flow().edges[2].target.has_value(),
                  "consumer: edge[2] terminal");
        }
    }

    if (g_failures != 0) {
        std::cerr << "\n" << g_failures << " failure(s) in ast-dump pin\n";
        return 1;
    }
    std::cerr << "ast-dump pin: producer + consumer OK\n";
    return 0;
}
