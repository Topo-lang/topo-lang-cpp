// Unit tests for CppSymbolAccessExtractor — global write detection.
//
// Focused regression coverage for the Pass-1 single-line-body handling: a
// function body that opens AND closes on one line must not latch `inFunction`
// true for the rest of the file, which previously dropped every later global
// write (a purity-check false negative).

#include "analysis/extract/CppSymbolAccessExtractor.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace topo::check;

#ifdef _WIN32
#include <process.h>
static int topo_getpid() {
    return _getpid();
}
#else
#include <unistd.h>
static int topo_getpid() {
    return getpid();
}
#endif

class CppSymbolAccessExtractorTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = fs::temp_directory_path() / ("topo_access_test_" + std::to_string(topo_getpid()));
        fs::create_directories(tempDir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
    }

    std::string writeTempFile(const std::string& name, const std::string& content) {
        auto path = tempDir_ / name;
        std::ofstream ofs(path);
        ofs << content;
        return path.string();
    }

    fs::path tempDir_;
};

static bool hasWrite(const std::vector<SymbolAccess>& v, const std::string& func, const std::string& sym) {
    for (const auto& a : v) {
        if (a.isWrite && a.function == func && a.symbol == sym) return true;
    }
    return false;
}

// A simple global write inside a normal multi-line body is detected.
TEST_F(CppSymbolAccessExtractorTest, BasicGlobalWrite) {
    auto path = writeTempFile("basic.cpp",
        "static int counter;\n"
        "void bump() {\n"
        "    counter = 1;\n"
        "}\n");

    CppSymbolAccessExtractor extractor;
    auto accesses = extractor.extractSymbolAccesses(path);

    EXPECT_TRUE(hasWrite(accesses, "bump", "counter"));
}

// A function whose body opens and closes on ONE line must not leak function
// scope into the following function — the later global write must still be
// detected. Before the fix `inFunction` latched forever and dropped it.
TEST_F(CppSymbolAccessExtractorTest, SingleLineBodyDoesNotLeakScope) {
    auto path = writeTempFile("single_line.cpp",
        "static int counter;\n"
        "int noop(int v) { return v; }\n"   // single-line body
        "void bump() {\n"
        "    counter += 1;\n"                // must still be seen
        "}\n");

    CppSymbolAccessExtractor extractor;
    auto accesses = extractor.extractSymbolAccesses(path);

    EXPECT_TRUE(hasWrite(accesses, "bump", "counter"))
        << "global write after a single-line-body function was dropped";
}

// Multiple single-line bodies in a row must not accumulate a latched scope.
TEST_F(CppSymbolAccessExtractorTest, MultipleSingleLineBodies) {
    auto path = writeTempFile("multi_single.cpp",
        "static int g;\n"
        "int a() { return 1; }\n"
        "int b() { return 2; }\n"
        "void writer() {\n"
        "    g = 5;\n"
        "}\n");

    CppSymbolAccessExtractor extractor;
    auto accesses = extractor.extractSymbolAccesses(path);

    EXPECT_TRUE(hasWrite(accesses, "writer", "g"));
}
