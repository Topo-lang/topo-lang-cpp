// Integration tests for CppImportExtractor and CppCallSiteExtractor.
// These tests create real C++ files on disk and run the extractors,
// verifying correctness of regex-based extraction against adversarial inputs.

#include "analysis/extract/CppImportExtractor.h"
#include "analysis/extract/CppCallSiteExtractor.h"
#include "topo/Check/CapabilityCatalog.h"

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

// ---------------------------------------------------------------------------
// Fixture: creates a temp directory, provides a helper to write files.
// ---------------------------------------------------------------------------
class CppExtractorTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = fs::temp_directory_path() /
                   ("topo_extractor_integ_" + std::to_string(topo_getpid()));
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

// ===========================================================================
// CppImportExtractor tests
// ===========================================================================

TEST_F(CppExtractorTest, Import_BasicIncludes) {
    auto path = writeTempFile("basic.cpp",
        "#include <sys/socket.h>\n"
        "#include \"fstream\"\n"
        "#include <vector>\n"
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    ASSERT_EQ(imports.size(), 3u);
    EXPECT_EQ(imports[0].normalizedPath, "sys/socket.h");
    EXPECT_EQ(imports[0].line, 1);
    EXPECT_EQ(imports[0].file, path);
    EXPECT_EQ(imports[1].normalizedPath, "fstream");
    EXPECT_EQ(imports[1].line, 2);
    EXPECT_EQ(imports[2].normalizedPath, "vector");
    EXPECT_EQ(imports[2].line, 3);
}

TEST_F(CppExtractorTest, Import_IncludeInBlockComment) {
    // Block-comment tracking now properly skips #include inside /* */.
    auto path = writeTempFile("block_comment.cpp",
        "/*\n"
        "#include <sys/socket.h>\n"
        "*/\n"
        "#include <vector>\n"
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    // The block-commented #include is correctly skipped.
    ASSERT_EQ(imports.size(), 1u);
    EXPECT_EQ(imports[0].normalizedPath, "vector");
    EXPECT_EQ(imports[0].line, 4);
}

TEST_F(CppExtractorTest, Import_SingleLineBlockCommentSameLine) {
    // /* #include <stdio.h> */ on the same line.
    // The regex anchors with ^ so the leading /* does not prevent matching.
    // This is another aspect of the block-comment limitation.
    auto path = writeTempFile("single_line_block.cpp",
        "/* #include <stdio.h> */\n"
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    // The regex requires ^\s*# -- the leading /* means # is not preceded only
    // by whitespace. So this should NOT match.
    EXPECT_EQ(imports.size(), 0u);
}

TEST_F(CppExtractorTest, Import_IncludeWithExtraSpaces) {
    auto path = writeTempFile("spaces.cpp",
        "#  include  <sys/socket.h>\n"
        "  #include   \"myheader.h\"\n"
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    ASSERT_EQ(imports.size(), 2u);
    EXPECT_EQ(imports[0].normalizedPath, "sys/socket.h");
    EXPECT_EQ(imports[0].line, 1);
    EXPECT_EQ(imports[1].normalizedPath, "myheader.h");
    EXPECT_EQ(imports[1].line, 2);
}

TEST_F(CppExtractorTest, Import_EmptyFile) {
    auto path = writeTempFile("empty.cpp", "");

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    EXPECT_TRUE(imports.empty());
}

TEST_F(CppExtractorTest, Import_NonexistentFile) {
    auto path = (tempDir_ / "nonexistent.cpp").string();

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    EXPECT_TRUE(imports.empty());
}

TEST_F(CppExtractorTest, Import_ExtractAll_MultipleFiles) {
    auto path1 = writeTempFile("a.cpp",
        "#include <vector>\n"
        "#include <string>\n"
    );
    auto path2 = writeTempFile("b.cpp",
        "#include <sys/socket.h>\n"
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractAll({path1, path2});

    ASSERT_EQ(imports.size(), 3u);
    // First two from file a.cpp, third from file b.cpp
    EXPECT_EQ(imports[0].file, path1);
    EXPECT_EQ(imports[0].normalizedPath, "vector");
    EXPECT_EQ(imports[1].file, path1);
    EXPECT_EQ(imports[1].normalizedPath, "string");
    EXPECT_EQ(imports[2].file, path2);
    EXPECT_EQ(imports[2].normalizedPath, "sys/socket.h");
}

TEST_F(CppExtractorTest, Import_ExtractAll_EmptyList) {
    CppImportExtractor extractor;
    auto imports = extractor.extractAll({});

    EXPECT_TRUE(imports.empty());
}

TEST_F(CppExtractorTest, Import_LineNumberAccuracy) {
    // Includes are interspersed with blank lines and code.
    auto path = writeTempFile("lineno.cpp",
        "\n"                             // line 1
        "#include <iostream>\n"          // line 2
        "\n"                             // line 3
        "int x = 0;\n"                   // line 4
        "\n"                             // line 5
        "#include <fstream>\n"           // line 6
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    ASSERT_EQ(imports.size(), 2u);
    EXPECT_EQ(imports[0].line, 2);
    EXPECT_EQ(imports[1].line, 6);
}

TEST_F(CppExtractorTest, Import_LineCommentedInclude) {
    // #include preceded by // on the same line.
    // The regex anchors with ^\s*# so a leading // will prevent the match.
    auto path = writeTempFile("line_comment.cpp",
        "// #include <stdlib.h>\n"
        "#include <vector>\n"
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    ASSERT_EQ(imports.size(), 1u);
    EXPECT_EQ(imports[0].normalizedPath, "vector");
}

// ===========================================================================
// CppCallSiteExtractor tests
// ===========================================================================

TEST_F(CppExtractorTest, CallSite_BasicFunctionWithApiCall) {
    auto path = writeTempFile("basic_call.cpp",
        "void handler() {\n"
        "    system(\"ls\");\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].callerQualifiedName, "handler");
    EXPECT_EQ(sites[0].calleePattern, "system");
    EXPECT_EQ(sites[0].capability, CapabilityKind::Process);
    EXPECT_EQ(sites[0].file, path);
    EXPECT_EQ(sites[0].line, 2);
}

TEST_F(CppExtractorTest, CallSite_ApiCallOutsideFunction) {
    // File-scope calls are not inside a function body, so they should
    // not be detected.
    auto path = writeTempFile("file_scope.cpp",
        "int x = socket(AF_INET, SOCK_STREAM, 0);\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    EXPECT_TRUE(sites.empty());
}

TEST_F(CppExtractorTest, CallSite_NamespacedFunction) {
    auto path = writeTempFile("namespaced.cpp",
        "namespace net {\n"
        "void connect_impl() {\n"
        "    socket(AF_INET, SOCK_STREAM, 0);\n"
        "}\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].callerQualifiedName, "net::connect_impl");
    EXPECT_EQ(sites[0].calleePattern, "socket");
    EXPECT_EQ(sites[0].capability, CapabilityKind::Network);
}

TEST_F(CppExtractorTest, CallSite_ClassMethod) {
    auto path = writeTempFile("class_method.cpp",
        "class Server {\n"
        "void start() {\n"
        "    bind(fd, addr, len);\n"
        "}\n"
        "};\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].callerQualifiedName, "Server::start");
    EXPECT_EQ(sites[0].calleePattern, "bind");
    EXPECT_EQ(sites[0].capability, CapabilityKind::Network);
}

TEST_F(CppExtractorTest, CallSite_MultipleCallsInOneFunction) {
    auto path = writeTempFile("multi_call.cpp",
        "void setup() {\n"
        "    socket(AF_INET, SOCK_STREAM, 0);\n"
        "    bind(fd, addr, len);\n"
        "    listen(fd, 128);\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 3u);
    EXPECT_EQ(sites[0].callerQualifiedName, "setup");
    EXPECT_EQ(sites[0].calleePattern, "socket");
    EXPECT_EQ(sites[1].callerQualifiedName, "setup");
    EXPECT_EQ(sites[1].calleePattern, "bind");
    EXPECT_EQ(sites[2].callerQualifiedName, "setup");
    EXPECT_EQ(sites[2].calleePattern, "listen");
    // All three are Network capability
    for (const auto& s : sites) {
        EXPECT_EQ(s.capability, CapabilityKind::Network);
    }
}

TEST_F(CppExtractorTest, CallSite_SafeApiNotDetected) {
    // std::sort is not in the CapabilityCatalog's API list.
    auto path = writeTempFile("safe_api.cpp",
        "void process() {\n"
        "    std::sort(v.begin(), v.end());\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    EXPECT_TRUE(sites.empty());
}

TEST_F(CppExtractorTest, CallSite_LineCommentSkipped) {
    // The extractor's isCommentLine() skips lines that start with //.
    // Also, mid-line // stops brace and API scanning.
    auto path = writeTempFile("line_comment.cpp",
        "void fn() {\n"
        "    // system(\"ls\");\n"
        "    int x = 1;\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    EXPECT_TRUE(sites.empty());
}

TEST_F(CppExtractorTest, CallSite_StringLiteralNotACall) {
    // "fopen is a function" does not contain fopen\s*\( so no match.
    auto path = writeTempFile("string_literal.cpp",
        "void fn() {\n"
        "    const char* s = \"fopen is a function\";\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    EXPECT_TRUE(sites.empty());
}

TEST_F(CppExtractorTest, CallSite_EmptyFile) {
    auto path = writeTempFile("empty.cpp", "");

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    EXPECT_TRUE(sites.empty());
}

TEST_F(CppExtractorTest, CallSite_NonexistentFile) {
    auto path = (tempDir_ / "nonexistent.cpp").string();

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    EXPECT_TRUE(sites.empty());
}

TEST_F(CppExtractorTest, CallSite_NamespacedClassMethod) {
    auto path = writeTempFile("ns_class.cpp",
        "namespace io {\n"
        "class FileReader {\n"
        "void open_file() {\n"
        "    fopen(\"data.txt\", \"r\");\n"
        "}\n"
        "};\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].callerQualifiedName, "io::FileReader::open_file");
    EXPECT_EQ(sites[0].calleePattern, "fopen");
    EXPECT_EQ(sites[0].capability, CapabilityKind::File);
}

TEST_F(CppExtractorTest, CallSite_MultipleCapabilityKinds) {
    // A function that uses both file and process APIs.
    auto path = writeTempFile("mixed_cap.cpp",
        "void do_stuff() {\n"
        "    fopen(\"x\", \"r\");\n"
        "    system(\"cmd\");\n"
        "    dlopen(\"lib.so\", 0);\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 3u);
    EXPECT_EQ(sites[0].capability, CapabilityKind::File);
    EXPECT_EQ(sites[1].capability, CapabilityKind::Process);
    EXPECT_EQ(sites[2].capability, CapabilityKind::DynamicLoad);
}

TEST_F(CppExtractorTest, CallSite_TwoFunctionsIsolated) {
    // Each function's calls should be attributed to the correct caller.
    auto path = writeTempFile("two_funcs.cpp",
        "void net_setup() {\n"
        "    socket(AF_INET, SOCK_STREAM, 0);\n"
        "}\n"
        "void file_setup() {\n"
        "    fopen(\"config.txt\", \"r\");\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 2u);
    EXPECT_EQ(sites[0].callerQualifiedName, "net_setup");
    EXPECT_EQ(sites[0].calleePattern, "socket");
    EXPECT_EQ(sites[1].callerQualifiedName, "file_setup");
    EXPECT_EQ(sites[1].calleePattern, "fopen");
}

TEST_F(CppExtractorTest, CallSite_MultipleCallsOnSameLine) {
    // Two API calls on the same line; the extractor iterates regex matches.
    auto path = writeTempFile("same_line.cpp",
        "void work() {\n"
        "    socket(0, 0, 0); bind(fd, addr, len);\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 2u);
    EXPECT_EQ(sites[0].calleePattern, "socket");
    EXPECT_EQ(sites[1].calleePattern, "bind");
    // Both report the same line number
    EXPECT_EQ(sites[0].line, 2);
    EXPECT_EQ(sites[1].line, 2);
}

TEST_F(CppExtractorTest, CallSite_InlineCommentAfterCode) {
    // API call followed by a // comment on the same line.
    // The call itself is before the comment, so it should be detected.
    auto path = writeTempFile("inline_comment.cpp",
        "void fn() {\n"
        "    system(\"ls\"); // run ls\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].calleePattern, "system");
}

TEST_F(CppExtractorTest, CallSite_ApiCallInCommentAfterCode) {
    // Code first, then a // comment containing an API call.
    // The API call is only in the comment portion after //.
    // The extractor now strips line-comment suffixes before API scanning,
    // so system("ls") in the comment is correctly ignored.
    auto path = writeTempFile("comment_after.cpp",
        "void fn() {\n"
        "    int x = 1; // system(\"ls\");\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    // The API regex operates on the effective line (comment stripped).
    EXPECT_TRUE(sites.empty());
}

TEST_F(CppExtractorTest, CallSite_ExecFamilyDetected) {
    // The exec* family is matched by the apiCallRegex explicit list
    // (execl, execv, execvp, execle, execve, execlp) and also by
    // classifyApiCall's prefix match for "exec".
    auto path = writeTempFile("exec_family.cpp",
        "void spawn() {\n"
        "    execl(\"/bin/sh\", \"sh\", nullptr);\n"
        "    execvp(\"cmd\", args);\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 2u);
    EXPECT_EQ(sites[0].calleePattern, "execl");
    EXPECT_EQ(sites[0].capability, CapabilityKind::Process);
    EXPECT_EQ(sites[1].calleePattern, "execvp");
    EXPECT_EQ(sites[1].capability, CapabilityKind::Process);
}

// ===========================================================================
// Import extractor: raw string and block comment robustness
// ===========================================================================

TEST_F(CppExtractorTest, Import_IncludeInRawString) {
    // #include inside R"(...)" raw string should be ignored.
    auto path = writeTempFile("raw_string.cpp",
        "const char* code = R\"(\n"
        "#include <sys/socket.h>\n"
        ")\";\n"
        "#include <vector>\n"
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    ASSERT_EQ(imports.size(), 1u);
    EXPECT_EQ(imports[0].normalizedPath, "vector");
    EXPECT_EQ(imports[0].line, 4);
}

TEST_F(CppExtractorTest, Import_IncludeInRawStringWithDelimiter) {
    // #include inside R"delim(...)delim" raw string should be ignored.
    auto path = writeTempFile("raw_delim.cpp",
        "const char* s = R\"xyz(\n"
        "#include <stdlib.h>\n"
        ")xyz\";\n"
        "#include <string>\n"
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    ASSERT_EQ(imports.size(), 1u);
    EXPECT_EQ(imports[0].normalizedPath, "string");
    EXPECT_EQ(imports[0].line, 4);
}

TEST_F(CppExtractorTest, Import_NestedBlockComments) {
    // Multiple block comments interspersed with real includes.
    auto path = writeTempFile("nested_block.cpp",
        "#include <vector>\n"
        "/*\n"
        "#include <stdlib.h>\n"
        "*/\n"
        "#include <string>\n"
        "/* #include <stdio.h> */\n"
        "#include <array>\n"
    );

    CppImportExtractor extractor;
    auto imports = extractor.extractImports(path);

    ASSERT_EQ(imports.size(), 3u);
    EXPECT_EQ(imports[0].normalizedPath, "vector");
    EXPECT_EQ(imports[1].normalizedPath, "string");
    EXPECT_EQ(imports[2].normalizedPath, "array");
}

// ===========================================================================
// Call site extractor: block comment and raw string robustness
// ===========================================================================

TEST_F(CppExtractorTest, CallSite_BlockCommentSkipped) {
    // API call inside a block comment should be ignored.
    auto path = writeTempFile("block_comment_call.cpp",
        "void fn() {\n"
        "    /*\n"
        "    system(\"ls\");\n"
        "    */\n"
        "    fopen(\"x\", \"r\");\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].calleePattern, "fopen");
}

TEST_F(CppExtractorTest, CallSite_RawStringSkipped) {
    // API call pattern inside a raw string should be ignored.
    auto path = writeTempFile("raw_string_call.cpp",
        "void fn() {\n"
        "    const char* s = R\"(\n"
        "    system(\"evil\");\n"
        ")\";\n"
        "    fopen(\"real\", \"r\");\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].calleePattern, "fopen");
}

TEST_F(CppExtractorTest, CallSite_QualifiedApiCall) {
    // std::fopen and ::fopen should be detected just like bare fopen.
    auto path = writeTempFile("qualified_call.cpp",
        "void fn() {\n"
        "    std::fopen(\"x\", \"r\");\n"
        "    ::system(\"ls\");\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 2u);
    EXPECT_EQ(sites[0].calleePattern, "fopen");
    EXPECT_EQ(sites[0].capability, CapabilityKind::File);
    EXPECT_EQ(sites[1].calleePattern, "system");
    EXPECT_EQ(sites[1].capability, CapabilityKind::Process);
}

// ===========================================================================
// Call site extractor: preprocessor and macro robustness
// ===========================================================================

TEST_F(CppExtractorTest, CallSite_PreprocessorBracesIgnored) {
    // #ifdef/#endif lines with braces should not corrupt brace depth.
    auto path = writeTempFile("preproc_braces.cpp",
        "void fn() {\n"
        "#ifdef USE_FEATURE\n"
        "    fopen(\"x\", \"r\");\n"
        "#endif\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].calleePattern, "fopen");
    EXPECT_EQ(sites[0].callerQualifiedName, "fn");
}

TEST_F(CppExtractorTest, CallSite_MacroDefineSkipped) {
    // A #define line should not be matched by funcDefRegex, but API calls
    // in the macro body ARE detected with a <macro:NAME> caller (F1.3).
    auto path = writeTempFile("macro_func.cpp",
        "#define MY_FUNC(x) { system(x); }\n"
        "void real_fn() {\n"
        "    fopen(\"x\", \"r\");\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 2u);
    EXPECT_EQ(sites[0].calleePattern, "system");
    EXPECT_EQ(sites[0].callerQualifiedName, "<macro:MY_FUNC>");
    EXPECT_EQ(sites[0].capability, CapabilityKind::Process);
    EXPECT_EQ(sites[1].calleePattern, "fopen");
    EXPECT_EQ(sites[1].callerQualifiedName, "real_fn");
    EXPECT_EQ(sites[1].capability, CapabilityKind::File);
}

TEST_F(CppExtractorTest, CallSite_NegativeBraceDepthClamped) {
    // An extra } at file scope should not cause negative braceDepth.
    // The extractor should recover and still track the next function.
    auto path = writeTempFile("extra_brace.cpp",
        "}\n"  // extra closing brace
        "void fn() {\n"
        "    socket(0, 0, 0);\n"
        "}\n"
    );

    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites(path);

    ASSERT_EQ(sites.size(), 1u);
    EXPECT_EQ(sites[0].calleePattern, "socket");
    EXPECT_EQ(sites[0].callerQualifiedName, "fn");
}
