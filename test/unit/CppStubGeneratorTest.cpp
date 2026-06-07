// Unit tests for CppStubGenerator — function body finding and stubbing.

#include "analysis/stub/CppStubGenerator.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// --- findFunctionBodyStart tests ---

TEST(CppStubGenerator, FindSimpleFunction) {
    std::string source =
        "int add(int a, int b) {\n"
        "    return a + b;\n"
        "}\n";
    size_t pos = CppStubGenerator::findFunctionBodyStart(source, "add");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(source[pos], '{');
}

TEST(CppStubGenerator, FindVoidFunction) {
    std::string source =
        "void doWork() {\n"
        "    // nothing\n"
        "}\n";
    size_t pos = CppStubGenerator::findFunctionBodyStart(source, "doWork");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(source[pos], '{');
}

TEST(CppStubGenerator, FindFunctionWithQualifiers) {
    std::string source =
        "int getValue() const noexcept {\n"
        "    return 42;\n"
        "}\n";
    size_t pos = CppStubGenerator::findFunctionBodyStart(source, "getValue");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(source[pos], '{');
}

TEST(CppStubGenerator, FindFunctionWithTrailingReturn) {
    std::string source =
        "auto compute(int x) -> int {\n"
        "    return x * 2;\n"
        "}\n";
    size_t pos = CppStubGenerator::findFunctionBodyStart(source, "compute");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(source[pos], '{');
}

TEST(CppStubGenerator, FindFunctionAmongMultiple) {
    std::string source =
        "void foo() { }\n"
        "int bar(int x) {\n"
        "    return x;\n"
        "}\n"
        "void baz() { }\n";
    size_t pos = CppStubGenerator::findFunctionBodyStart(source, "bar");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(source[pos], '{');

    // Verify it's the correct function by checking the content before
    std::string before = source.substr(0, pos);
    EXPECT_NE(before.find("bar"), std::string::npos);
}

TEST(CppStubGenerator, DoesNotMatchSubstring) {
    std::string source =
        "int foobar(int x) {\n"
        "    return x;\n"
        "}\n";
    // "foo" should not match "foobar"
    size_t pos = CppStubGenerator::findFunctionBodyStart(source, "foo");
    EXPECT_EQ(pos, std::string::npos);
}

TEST(CppStubGenerator, DoesNotMatchDeclaration) {
    std::string source =
        "int compute(int x);\n" // declaration, no body
        "void other() { }\n";
    size_t pos = CppStubGenerator::findFunctionBodyStart(source, "compute");
    EXPECT_EQ(pos, std::string::npos);
}

TEST(CppStubGenerator, FindFunctionNotFound) {
    std::string source = "void foo() { }\n";
    size_t pos = CppStubGenerator::findFunctionBodyStart(source, "nonexistent");
    EXPECT_EQ(pos, std::string::npos);
}

// --- findMatchingBrace tests ---

TEST(CppStubGenerator, MatchingBraceSimple) {
    std::string source = "{ return 42; }";
    size_t end = CppStubGenerator::findMatchingBrace(source, 0);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(end, source.size() - 1);
}

TEST(CppStubGenerator, MatchingBraceNested) {
    std::string source = "{ if (x) { y(); } return z; }";
    size_t end = CppStubGenerator::findMatchingBrace(source, 0);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(source[end], '}');
    EXPECT_EQ(end, source.size() - 1);
}

TEST(CppStubGenerator, MatchingBraceWithStrings) {
    std::string source = "{ std::string s = \"}\"; return s; }";
    size_t end = CppStubGenerator::findMatchingBrace(source, 0);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(source[end], '}');
    EXPECT_EQ(end, source.size() - 1);
}

TEST(CppStubGenerator, MatchingBraceWithComments) {
    std::string source = "{ // closing brace: }\n  return 1; }";
    size_t end = CppStubGenerator::findMatchingBrace(source, 0);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(source[end], '}');
    EXPECT_EQ(end, source.size() - 1);
}

TEST(CppStubGenerator, MatchingBraceBlockComment) {
    std::string source = "{ /* } */ return 1; }";
    size_t end = CppStubGenerator::findMatchingBrace(source, 0);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(source[end], '}');
    EXPECT_EQ(end, source.size() - 1);
}

TEST(CppStubGenerator, MatchingBraceUnmatched) {
    std::string source = "{ return 1;";
    size_t end = CppStubGenerator::findMatchingBrace(source, 0);
    EXPECT_EQ(end, std::string::npos);
}

// A C++14 digit separator (`1'000`) must not be mistaken for a char-literal
// opener, which would scan to the next `'` and swallow the closing brace.
TEST(CppStubGenerator, MatchingBraceDigitSeparator) {
    std::string source = "{ int n = 1'000'000; return n; }";
    size_t end = CppStubGenerator::findMatchingBrace(source, 0);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(source[end], '}');
    EXPECT_EQ(end, source.size() - 1);
}

// Hex digit separators (`0xFF'FF`) likewise are not char literals.
TEST(CppStubGenerator, MatchingBraceHexDigitSeparator) {
    std::string source = "{ unsigned m = 0xFF'FF; return m; }";
    size_t end = CppStubGenerator::findMatchingBrace(source, 0);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(end, source.size() - 1);
}

// A genuine char literal must still be skipped (so a `}` inside it does not
// terminate the body early).
TEST(CppStubGenerator, MatchingBraceCharLiteralBrace) {
    std::string source = "{ char c = '}'; return c; }";
    size_t end = CppStubGenerator::findMatchingBrace(source, 0);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(end, source.size() - 1);
}

// --- isVoidReturn tests ---

TEST(CppStubGenerator, IsVoidReturnTrue) {
    std::string source = "void doWork() {";
    size_t bodyPos = source.find('{');
    EXPECT_TRUE(CppStubGenerator::isVoidReturn(source, bodyPos));
}

TEST(CppStubGenerator, IsVoidReturnFalse) {
    std::string source = "int compute(int x) {";
    size_t bodyPos = source.find('{');
    EXPECT_FALSE(CppStubGenerator::isVoidReturn(source, bodyPos));
}

// A preceding void function must not bleed into the next function's return
// type. The old window scan saw the earlier `void` and misclassified `make`.
TEST(CppStubGenerator, IsVoidReturnNotLeakedFromPrevious) {
    std::string source =
        "void prev() {}\n"
        "Widget make() {";
    size_t bodyPos = source.rfind('{');
    EXPECT_FALSE(CppStubGenerator::isVoidReturn(source, bodyPos));
}

// `void*` as a return type is NOT a void return (a stub must `return {};`).
TEST(CppStubGenerator, IsVoidReturnPointerNotVoid) {
    std::string source = "void* allocate(size_t n) {";
    size_t bodyPos = source.find('{');
    EXPECT_FALSE(CppStubGenerator::isVoidReturn(source, bodyPos));
}

// A `void()` buried in a template return type (`std::function<void()>`) must
// not be read as a void return.
TEST(CppStubGenerator, IsVoidReturnInTemplateArgNotVoid) {
    std::string source = "std::function<void()> handler() {";
    size_t bodyPos = source.find('{');
    EXPECT_FALSE(CppStubGenerator::isVoidReturn(source, bodyPos));
}

// A `void*` parameter must not make the function look void-returning.
TEST(CppStubGenerator, IsVoidReturnVoidParamNotVoid) {
    std::string source = "int handle(void* ctx) {";
    size_t bodyPos = source.find('{');
    EXPECT_FALSE(CppStubGenerator::isVoidReturn(source, bodyPos));
}

// --- Integration: stub + restore via temp file ---

class CppStubGeneratorFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = fs::temp_directory_path() / ("topo_check_test_" + std::to_string(topo_getpid()));
        fs::create_directories(tempDir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
    }

    std::string writeTempFile(const std::string& name, const std::string& content) {
        auto path = tempDir_ / name;
        // Binary mode: on Windows a text-mode stream translates '\n' -> '\r\n'
        // on disk, so the generator would read back CRLF and originalContent
        // would not byte-match the LF `content` the test wrote.
        std::ofstream ofs(path, std::ios::binary);
        ofs << content;
        return path.string();
    }

    std::string readTempFile(const std::string& path) {
        std::ifstream ifs(path, std::ios::binary);
        std::ostringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }

    fs::path tempDir_;
};

TEST_F(CppStubGeneratorFileTest, StubAndRestore) {
    std::string original =
        "int compute(int x) {\n"
        "    int y = x * 2;\n"
        "    return y + 1;\n"
        "}\n";
    std::string path = writeTempFile("test.cpp", original);

    CppStubGenerator gen;
    auto result = gen.stubFunction(path, "compute");

    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.originalContent, original);

    // File should now contain stub
    std::string modified = readTempFile(path);
    EXPECT_NE(modified, original);
    EXPECT_NE(modified.find("return {};"), std::string::npos);

    // Restore
    EXPECT_TRUE(gen.restoreFile(path, result));
    std::string restored = readTempFile(path);
    EXPECT_EQ(restored, original);
}

TEST_F(CppStubGeneratorFileTest, StubVoidFunction) {
    std::string original =
        "void process() {\n"
        "    doSomething();\n"
        "}\n";
    std::string path = writeTempFile("test.cpp", original);

    CppStubGenerator gen;
    auto result = gen.stubFunction(path, "process");

    ASSERT_TRUE(result.success) << result.error;

    std::string modified = readTempFile(path);
    EXPECT_NE(modified.find("{ }"), std::string::npos);
    // Should NOT contain "return"
    EXPECT_EQ(modified.find("return"), std::string::npos);
}

TEST_F(CppStubGeneratorFileTest, StubFunctionNotFound) {
    std::string original = "void foo() { }\n";
    std::string path = writeTempFile("test.cpp", original);

    CppStubGenerator gen;
    auto result = gen.stubFunction(path, "nonexistent");

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(CppStubGeneratorFileTest, StubNestedBraces) {
    std::string original =
        "int complex(int x) {\n"
        "    if (x > 0) {\n"
        "        for (int i = 0; i < x; ++i) {\n"
        "            x += i;\n"
        "        }\n"
        "    }\n"
        "    return x;\n"
        "}\n";
    std::string path = writeTempFile("test.cpp", original);

    CppStubGenerator gen;
    auto result = gen.stubFunction(path, "complex");

    ASSERT_TRUE(result.success) << result.error;

    std::string modified = readTempFile(path);
    EXPECT_NE(modified.find("return {};"), std::string::npos);
    // Original nested braces should be gone
    EXPECT_EQ(modified.find("for ("), std::string::npos);
}
