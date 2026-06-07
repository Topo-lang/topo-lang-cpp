// Unit tests for CppSymbolExtractor — symbol extraction from C++ source files.

#include "analysis/extract/CppSymbolExtractor.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace topo::check;
using topo::Visibility;

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

class CppSymbolExtractorTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = fs::temp_directory_path() / ("topo_extractor_test_" + std::to_string(topo_getpid()));
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

// Helper: find a symbol by simpleName in a vector.
static const HostSymbol* findByName(const std::vector<HostSymbol>& syms, const std::string& name) {
    for (const auto& s : syms) {
        if (s.simpleName == name) return &s;
    }
    return nullptr;
}

// Helper: find a symbol by qualifiedName in a vector.
static const HostSymbol* findByQualified(const std::vector<HostSymbol>& syms, const std::string& qname) {
    for (const auto& s : syms) {
        if (s.qualifiedName == qname) return &s;
    }
    return nullptr;
}

// 1. FreeFunction
TEST_F(CppSymbolExtractorTest, FreeFunction) {
    auto path = writeTempFile("free.cpp", "void doWork() { }\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "doWork");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->qualifiedName, "doWork");
    EXPECT_EQ(sym->kind, HostSymbolKind::Function);
    EXPECT_EQ(sym->returnType, "void");
}

// 2. NamespacedFunction
TEST_F(CppSymbolExtractorTest, NamespacedFunction) {
    auto path = writeTempFile("ns.cpp",
                              "namespace engine {\n"
                              "void init() { }\n"
                              "}\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "init");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->qualifiedName, "engine::init");
}

// 3. NestedNamespace
TEST_F(CppSymbolExtractorTest, NestedNamespace) {
    auto path = writeTempFile("nested_ns.cpp",
                              "namespace a {\n"
                              "namespace b {\n"
                              "void f() { }\n"
                              "}\n"
                              "}\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "f");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->qualifiedName, "a::b::f");
}

// 4. ClassMethod
TEST_F(CppSymbolExtractorTest, ClassMethod) {
    auto path = writeTempFile("class_method.cpp",
                              "class Foo {\n"
                              "void bar() { }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "bar");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->kind, HostSymbolKind::Method);
    EXPECT_EQ(sym->enclosingClass, "Foo");
    EXPECT_EQ(sym->qualifiedName, "Foo::bar");
}

// 5. NamespacedClassMethod
TEST_F(CppSymbolExtractorTest, NamespacedClassMethod) {
    auto path = writeTempFile("ns_class.cpp",
                              "namespace ns {\n"
                              "class C {\n"
                              "void m() { }\n"
                              "};\n"
                              "}\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "m");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->qualifiedName, "ns::C::m");
}

// 6. ConstructorDetected — explicit constructor
TEST_F(CppSymbolExtractorTest, ConstructorDetected) {
    auto path = writeTempFile("ctor.cpp",
                              "class Foo {\n"
                              "explicit Foo() { }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByQualified(syms, "Foo::Foo");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->kind, HostSymbolKind::Constructor);
    EXPECT_EQ(sym->simpleName, "Foo");
}

// 6b. BareConstructorDetected — constructor without prefix keyword
TEST_F(CppSymbolExtractorTest, BareConstructorDetected) {
    auto path = writeTempFile("bare_ctor.cpp",
                              "class Foo {\n"
                              "Foo() { }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByQualified(syms, "Foo::Foo");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->kind, HostSymbolKind::Constructor);
    EXPECT_EQ(sym->simpleName, "Foo");
}

// 7. DestructorDetected
TEST_F(CppSymbolExtractorTest, DestructorDetected) {
    auto path = writeTempFile("dtor.cpp",
                              "class Foo {\n"
                              "~Foo() { }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByQualified(syms, "Foo::~Foo");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->kind, HostSymbolKind::Destructor);
    EXPECT_EQ(sym->simpleName, "~Foo");
}

// 8. StaticMethodDetected
TEST_F(CppSymbolExtractorTest, StaticMethodDetected) {
    auto path = writeTempFile("static.cpp",
                              "class Foo {\n"
                              "static void bar() { }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "bar");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->kind, HostSymbolKind::StaticMethod);
    EXPECT_TRUE(sym->isStatic);
}

// 9. ConstMethodDetected
TEST_F(CppSymbolExtractorTest, ConstMethodDetected) {
    auto path = writeTempFile("const.cpp",
                              "class Foo {\n"
                              "int get() const { return 0; }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "get");
    ASSERT_NE(sym, nullptr);
    EXPECT_TRUE(sym->isConst);
}

// 10. ClassVisibilityDefault
TEST_F(CppSymbolExtractorTest, ClassVisibilityDefault) {
    auto path = writeTempFile("class_default.cpp",
                              "class Foo {\n"
                              "void bar() { }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "bar");
    ASSERT_NE(sym, nullptr);
    ASSERT_TRUE(sym->hostVisibility.has_value());
    EXPECT_EQ(sym->hostVisibility.value(), Visibility::Private);
}

// 11. StructVisibilityDefault
TEST_F(CppSymbolExtractorTest, StructVisibilityDefault) {
    auto path = writeTempFile("struct_default.cpp",
                              "struct S {\n"
                              "void bar() { }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "bar");
    ASSERT_NE(sym, nullptr);
    ASSERT_TRUE(sym->hostVisibility.has_value());
    EXPECT_EQ(sym->hostVisibility.value(), Visibility::Public);
}

// 12. AccessSpecifierChange
TEST_F(CppSymbolExtractorTest, AccessSpecifierChange) {
    auto path = writeTempFile("access.cpp",
                              "class Foo {\n"
                              "public:\n"
                              "void a() { }\n"
                              "private:\n"
                              "void b() { }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* symA = findByName(syms, "a");
    ASSERT_NE(symA, nullptr);
    ASSERT_TRUE(symA->hostVisibility.has_value());
    EXPECT_EQ(symA->hostVisibility.value(), Visibility::Public);

    auto* symB = findByName(syms, "b");
    ASSERT_NE(symB, nullptr);
    ASSERT_TRUE(symB->hostVisibility.has_value());
    EXPECT_EQ(symB->hostVisibility.value(), Visibility::Private);
}

// 13. ClassSymbolRecorded
TEST_F(CppSymbolExtractorTest, ClassSymbolRecorded) {
    auto path = writeTempFile("class_sym.cpp",
                              "class Foo {\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "Foo");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->kind, HostSymbolKind::Class);
}

// 14. StructSymbolRecorded
TEST_F(CppSymbolExtractorTest, StructSymbolRecorded) {
    auto path = writeTempFile("struct_sym.cpp",
                              "struct Bar {\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "Bar");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->kind, HostSymbolKind::Struct);
}

// 15. KeywordNotExtracted
TEST_F(CppSymbolExtractorTest, KeywordNotExtracted) {
    auto path = writeTempFile("keywords.cpp",
                              "void foo() {\n"
                              "    if (x) { }\n"
                              "    for (;;) { }\n"
                              "}\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    // Only "foo" should be extracted as a function
    bool foundFoo = false;
    for (const auto& s : syms) {
        EXPECT_NE(s.simpleName, "if") << "keyword 'if' should not be extracted";
        EXPECT_NE(s.simpleName, "for") << "keyword 'for' should not be extracted";
        EXPECT_NE(s.simpleName, "while") << "keyword 'while' should not be extracted";
        if (s.simpleName == "foo") foundFoo = true;
    }
    EXPECT_TRUE(foundFoo);
}

// 16. EmptyFileReturnsEmpty
TEST_F(CppSymbolExtractorTest, EmptyFileReturnsEmpty) {
    auto path = writeTempFile("empty.cpp", "");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    EXPECT_TRUE(syms.empty());
}

// 17. NonexistentFileReturnsEmpty
TEST_F(CppSymbolExtractorTest, NonexistentFileReturnsEmpty) {
    auto path = (tempDir_ / "nonexistent.cpp").string();

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    EXPECT_TRUE(syms.empty());
}

// 18. NestedClass
TEST_F(CppSymbolExtractorTest, NestedClass) {
    auto path = writeTempFile("nested_class.cpp",
                              "namespace ns {\n"
                              "class Outer {\n"
                              "class Inner {\n"
                              "void m() { }\n"
                              "};\n"
                              "};\n"
                              "}\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "m");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->qualifiedName, "ns::Outer::Inner::m");
}

// 19. ReturnTypeAndParams — verify type extraction
TEST_F(CppSymbolExtractorTest, ReturnTypeAndParams) {
    auto path = writeTempFile("types.cpp", "int compute(double x, const std::string& name) { return 0; }\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "compute");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->returnType, "int");
    ASSERT_EQ(sym->paramTypes.size(), 2u);
    EXPECT_EQ(sym->paramTypes[0], "double");
    EXPECT_EQ(sym->paramTypes[1], "const std::string&");
}

// 20. VoidReturnNoParams
TEST_F(CppSymbolExtractorTest, VoidReturnNoParams) {
    auto path = writeTempFile("voidfn.cpp", "void doNothing() { }\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "doNothing");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->returnType, "void");
    EXPECT_TRUE(sym->paramTypes.empty());
}

// 21a. BraceInStringDoesNotPopScope — a `}` inside a string literal must not
// close the enclosing namespace early, which would mis-qualify later symbols.
TEST_F(CppSymbolExtractorTest, BraceInStringDoesNotPopScope) {
    auto path = writeTempFile("brace_in_string.cpp",
                              "namespace app {\n"
                              "const char* tmpl = \"}\";\n"
                              "void run() { }\n"
                              "}\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "run");
    ASSERT_NE(sym, nullptr);
    // If the `}` in the string had popped the namespace, this would be "run".
    EXPECT_EQ(sym->qualifiedName, "app::run");
}

// 21b. BraceInCommentDoesNotPopScope — a `}` inside a line comment must not
// close scope either.
TEST_F(CppSymbolExtractorTest, BraceInCommentDoesNotPopScope) {
    auto path = writeTempFile("brace_in_comment.cpp",
                              "namespace app {\n"
                              "// closing here }\n"
                              "void run() { }\n"
                              "}\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByName(syms, "run");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->qualifiedName, "app::run");
}

// 21. ConstructorWithParams
TEST_F(CppSymbolExtractorTest, ConstructorWithParams) {
    auto path = writeTempFile("ctor_params.cpp",
                              "class Widget {\n"
                              "explicit Widget(int width, int height) { }\n"
                              "};\n");

    CppSymbolExtractor extractor;
    auto syms = extractor.extractSymbols(path);

    auto* sym = findByQualified(syms, "Widget::Widget");
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->kind, HostSymbolKind::Constructor);
    EXPECT_TRUE(sym->returnType.empty());
    ASSERT_EQ(sym->paramTypes.size(), 2u);
    EXPECT_EQ(sym->paramTypes[0], "int");
    EXPECT_EQ(sym->paramTypes[1], "int");
}
