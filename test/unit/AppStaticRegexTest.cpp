// Regression coverage for topo-app's regex MVP static front-end
// (runtime/include/topo/app_static.h). The header is the documented,
// no-clang FALLBACK producer; its opening contract promises each known
// gap is surfaced "rather than silently mis-extracting". These tests pin
// the five shapes that previously violated that contract — see
// .aidesk/live/40-issue/app-static-regex-silent-misread-adjacent-literal.md:
//
//   1. adjacent string-literal handler / flow names ("re" "duce")
//   2. bare `char` parity with app.h's scalar_of<char>() (Int, not Str)
//   3. bare `unsigned` accepted as unsigned int (Int), not thrown
//   4. a second function definition on the same physical line is visible
//   5. a trailing comma in flow(...) does not emit a phantom empty edge
//
// Header-only suite (no LLVM, no clang invocation): the front-end reads
// C++ source TEXT and builds a topo::app::Graph in memory.

#include <topo/app_static.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using topo::app::Scalar;
using topo::app::stat::parse_app_source;
using topo::app::stat::scalar_from_cpp;
using topo::app::stat::join_adjacent_literals;

namespace {

// True iff the graph has a handler registered under exactly `name`.
bool has_handler(const topo::app::App& app, const std::string& name) {
    return app.graph().handler(name) != nullptr;
}

// Collect (source, target) edge pairs as strings for assertion; a
// terminal edge (target == nullopt) is rendered with target "<void>".
std::vector<std::pair<std::string, std::string>> edge_pairs(
    const topo::app::App& app) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!app.graph().has_flow()) return out;
    for (const auto& e : app.graph().flow().edges)
        out.emplace_back(e.source, e.target.value_or("<void>"));
    return out;
}

}  // namespace

// --- Fix 1: adjacent string-literal handler / flow names ------------------

TEST(AppStaticRegex, AdjacentHandlerLiteralIsJoinedNotTruncated) {
    // C++ concatenates `"re" "duce"` to "reduce"; the old regex captured
    // only "re" and silently dropped the rest, leaving the flow's "reduce"
    // stage with no matching handler and NO diagnostic.
    const std::string src = R"CPP(
        #include <topo/app.h>
        int reduce(int in) { return in; }
        void build() {
            topo::app::App app("orders");
            app.handler(&reduce, "re" "duce");
            app.flow("p", "reduce");
        }
    )CPP";
    topo::app::App app = parse_app_source(src);
    EXPECT_TRUE(has_handler(app, "reduce"))
        << "adjacent literals must join to the compiler's concatenation";
    EXPECT_FALSE(has_handler(app, "re"))
        << "the first fragment alone must not register a handler";
}

TEST(AppStaticRegex, AdjacentFlowAndStageLiteralsJoin) {
    // Flow name and a literal stage spelled as adjacent literals must read
    // the same joined form, so the stage still resolves to its handler.
    const std::string src = R"CPP(
        int reduce(int in) { return in; }
        void build() {
            topo::app::App app("orders");
            app.handler(&reduce, "reduce");
            app.flow("pi" "pe", "re" "duce");
        }
    )CPP";
    topo::app::App app = parse_app_source(src);
    ASSERT_TRUE(app.graph().has_flow());
    EXPECT_EQ(app.graph().flow().name, "pipe");
    // single stage -> one terminal edge from the joined stage name
    auto edges = edge_pairs(app);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].first, "reduce");
    EXPECT_EQ(edges[0].second, "<void>");
}

TEST(AppStaticRegex, JoinAdjacentLiteralsHelperCountsFragments) {
    int n = 0;
    auto joined = join_adjacent_literals(R"("a" "b" "c")", &n);
    ASSERT_TRUE(joined.has_value());
    EXPECT_EQ(*joined, "abc");
    EXPECT_EQ(n, 3);
    // A run stops at a non-whitespace separator (here a comma).
    n = 0;
    auto stop = join_adjacent_literals(R"("x" "y", "z")", &n);
    ASSERT_TRUE(stop.has_value());
    EXPECT_EQ(*stop, "xy");
    EXPECT_EQ(n, 2);
}

// --- Fix 2: bare char parity with app.h's scalar_of<char>() ---------------

TEST(AppStaticRegex, BareCharMapsToIntMatchingRuntimeBridge) {
    // app.h: char satisfies std::is_integral_v -> Scalar::Int. The static
    // reader previously listed bare `char` in the Str branch.
    EXPECT_EQ(scalar_from_cpp("char"), Scalar::Int);
    EXPECT_EQ(scalar_from_cpp("signed char"), Scalar::Int);
    EXPECT_EQ(scalar_from_cpp("unsigned char"), Scalar::Int);
}

TEST(AppStaticRegex, ConstCharPtrStillMapsToStr) {
    // The pointer-to-char string case must survive bare char becoming Int.
    EXPECT_EQ(scalar_from_cpp("const char*"), Scalar::Str);
    EXPECT_EQ(scalar_from_cpp("char *"), Scalar::Str);
}

// --- Fix 3: bare unsigned accepted as unsigned int ------------------------

TEST(AppStaticRegex, BareUnsignedMapsToIntNotThrows) {
    EXPECT_EQ(scalar_from_cpp("unsigned"), Scalar::Int);
    EXPECT_EQ(scalar_from_cpp("signed"), Scalar::Int);
    EXPECT_EQ(scalar_from_cpp("unsigned int"), Scalar::Int);
}

// --- Fix 4: a second definition on the same physical line is visible ------

TEST(AppStaticRegex, SecondFunctionOnSameLineIsVisible) {
    // `int a(int){...} int b(int){...}` on one line: the old fn regex
    // anchored on (?:^|\n) and dropped `b`, so a handler on &b threw
    // "no visible definition".
    const std::string src =
        "int a(int in){return in;} int b(int in){return in;}\n"
        "void build() {\n"
        "  topo::app::App app(\"orders\");\n"
        "  app.handler(&a, \"a\");\n"
        "  app.handler(&b, \"b\");\n"
        "  app.flow(\"p\", \"a\", \"b\");\n"
        "}\n";
    topo::app::App app = parse_app_source(src);
    EXPECT_TRUE(has_handler(app, "a"));
    EXPECT_TRUE(has_handler(app, "b"))
        << "the second same-line definition must be extracted";
}

// --- Fix 5: trailing comma in flow(...) yields no phantom empty edge ------

TEST(AppStaticRegex, TrailingCommaInFlowProducesNoEmptyEdge) {
    const std::string src = R"CPP(
        int a(int in) { return in; }
        void build() {
            topo::app::App app("orders");
            app.handler(&a, "a");
            app.flow("p", "a",);
        }
    )CPP";
    topo::app::App app = parse_app_source(src);
    auto edges = edge_pairs(app);
    // Exactly one terminal edge {a -> void}; NO {a -> ""} or { "" -> void }.
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].first, "a");
    EXPECT_EQ(edges[0].second, "<void>");
    for (const auto& e : edges) {
        EXPECT_FALSE(e.first.empty()) << "no edge may have an empty source";
        EXPECT_NE(e.second, "") << "no edge may have an empty target name";
    }
}

TEST(AppStaticRegex, TrailingCommaInsideParallelProducesNoEmptyMember) {
    const std::string src = R"CPP(
        int a(int in) { return in; }
        int b(int in) { return in; }
        int c(int in) { return in; }
        void build() {
            topo::app::App app("orders");
            app.handler(&a, "a");
            app.handler(&b, "b");
            app.handler(&c, "c");
            app.flow("p", "a", parallel("b", "c",));
        }
    )CPP";
    topo::app::App app = parse_app_source(src);
    auto edges = edge_pairs(app);
    // a -> b, a -> c, then terminal b -> void, c -> void. No empty-named
    // phantom from the trailing comma inside parallel(...).
    for (const auto& e : edges) {
        EXPECT_FALSE(e.first.empty());
        EXPECT_NE(e.second, "");
    }
    // fan-out from a reaches exactly b and c
    int fan = 0;
    for (const auto& e : edges)
        if (e.first == "a" && (e.second == "b" || e.second == "c")) ++fan;
    EXPECT_EQ(fan, 2);
}
