// E2E tests for C++ containment checks.

#include "CheckRunner.h"
#include "ClangdBridge.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace topo;

static std::string fixtureDir(const char* name) {
    return std::string(TOPO_TEST_FIXTURES_DIR) + "/" + name;
}

// Fixture for L2 (clangd-backed) deep containment tests.
//
// Skip semantics:
//   - clangd missing on the host  -> SetUp() emits a single suite-level
//     GTEST_SKIP. Environment-only blockers belong here, not inside each
//     test case (per project policy: per-case skips silently mask real
//     failures).
//   - clangd present but L2 returns rc != 0 -> the per-case test FAILS
//     with a diagnostic dump, never skips.
class CppContainmentL2 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!topo::lsp::ClangdBridge::isClangdAvailable()) {
            GTEST_SKIP() << "clangd unavailable (bundled TOPO_LLVM_BINDIR missing"
                            " and not on PATH) — L2 deep containment tests need"
                            " clangd to run.";
        }
    }
};

// --- Original containment tests ---

TEST(CppContainment, ContainmentFailFixture) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_fail");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentSystemCall) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_system_call");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentCastEscape) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_cast_escape");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentAsmEscape) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_asm_escape");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentVolatile) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_volatile");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentSetjmp) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_setjmp");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentGoto) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_goto");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentNetwork) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_network");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentDlopen) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_dlopen");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentThread) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_thread");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentDepInclude) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_dep_include");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentExternalOk) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_external_ok");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

TEST(CppContainment, ContainmentSafeCode) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_safe_code");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

// --- Structural detection ---

TEST(CppContainment, ContainmentCtorBackdoor) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_ctor_backdoor");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentDtorBackdoor) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_dtor_backdoor");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentGlobalInit) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_global_init");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentMacroHide) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_macro_hide");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentAttrConstructor) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_attr_constructor");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentExitCall) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_exit_call");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentMprotect) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_mprotect");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentSetuid) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_setuid");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentUnionPunning) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_union_punning");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentPlacementNew) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_placement_new");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

// --- API/escape expansion ---

TEST(CppContainment, ContainmentMultilineCast) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_multiline_cast");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentStdAsync) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_std_async");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentConditionalAttack) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_conditional_attack");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentHeaderBackdoor) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_header_backdoor");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentPragmaLink) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_pragma_link");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentCpp11Attr) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_cpp11_attr");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentAddrOfSystem) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_addr_of_system");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentGetenv) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_getenv");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentMultilineDefine) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_multiline_define");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentUserHeaderSafe) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_user_header_safe");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

TEST(CppContainment, ContainmentBuiltinDangerous) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_builtin_dangerous");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

// --- L2 deep analysis ---
//
// L2 deep analysis requires clangd. Tests that flip cfg.deepMode = true use
// the CppContainmentL2 fixture, which skips the entire suite once at SetUp
// when clangd is missing. Per-case GTEST_SKIP for analysis failures is
// prohibited (it silently masks regressions — see #1).

TEST_F(CppContainmentL2, ContainmentL2SafeStdlib) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_l2_safe_stdlib");
    cfg.checkName = "containment";
    cfg.deepMode = true;
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    // L2 or L1 fallback — safe code should pass either way
    EXPECT_EQ(runner.run(), 0);
}

TEST_F(CppContainmentL2, ContainmentL2UserFunctionSafe) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_l2_user_function_safe");
    cfg.checkName = "containment";
    cfg.deepMode = true;
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

// L1-only smoke tests — no clangd dependency, so they stay outside the
// CppContainmentL2 fixture even though their fixture names share the L2 prefix.
TEST(CppContainment, ContainmentL2NamespaceSystem) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_l2_namespace_system");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    // Even in L1 mode, ::system() should be detected
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentL2IndirectIo) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_l2_indirect_io");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    // iostream is not in safe headers — containment violation
    EXPECT_EQ(runner.run(), 1);
}

TEST_F(CppContainmentL2, ContainmentL2DeepOnly) {
    // This fixture has a user-defined app::read() that collides with the POSIX
    // read() name.  L2 (clangd) resolves the ambiguity correctly; L1 regex
    // cannot, so the test requires working L2 analysis. A non-zero rc here is
    // a real failure — never a skip — and must dump diagnostics.

    auto dir = fixtureDir("containment_l2_deep_only");

    // Generate compile_commands.json so clangd can index the fixture.
    {
        namespace fs = std::filesystem;
        fs::create_directories(fs::path(dir) / "build");
        std::ofstream ccj(fs::path(dir) / "build" / "compile_commands.json");
        ccj << "[{\"directory\": \"" << dir
            << "\", \"file\": \"src/main.cpp\", \"arguments\": "
            << "[\"clang++\", \"-std=c++17\", \"-c\", \"src/main.cpp\"]}]";
    }

    CheckConfig cfg;
    cfg.projectDir = dir;
    cfg.checkName = "containment";
    cfg.deepMode = true;
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());

    int rc = runner.run();
    if (rc != 0) {
        // Dump every diagnostic so the failure is actionable. The most
        // common cause is L2 silently falling through to L1 (which always
        // misclassifies app::read() as POSIX read()), so the per-diagnostic
        // text — including the "(level N)" tag and the caller token — is
        // exactly what a regression investigator needs.
        const auto& results = runner.lastResults();
        std::cerr << "ContainmentL2DeepOnly diagnostics ("
                  << results.size() << " check(s)):\n";
        for (const auto& [name, res] : results) {
            std::cerr << "  [" << name << "] errors=" << res.errorCount
                      << " warnings=" << res.warningCount << "\n";
            for (const auto& d : res.diagnostics) {
                std::cerr << "    - " << d.check << ": " << d.message;
                if (!d.file.empty()) {
                    std::cerr << " [" << d.file;
                    if (d.line > 0) std::cerr << ":" << d.line;
                    std::cerr << "]";
                }
                std::cerr << "\n";
            }
        }
    }
    // L2 must classify app::read() as the user's .topo-declared function and
    // return 0. Any other outcome is a regression — fail loudly.
    EXPECT_EQ(rc, 0)
        << "L2 deep containment failed for the app::read() vs POSIX read() "
           "fixture. This usually means L2 silently fell through to L1 "
           "(check for 'CppSafePatterns.toml not found' or 'clangd unavailable' "
           "diagnostics above).";
}

TEST_F(CppContainmentL2, ContainmentL2ExternalOk) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_l2_external_ok");
    cfg.checkName = "containment";
    cfg.deepMode = true;
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

// Regression guard for checker-l2-synthetic-caller-attribution: the memory
// fixture has three external functions (`allocator_alloc`, `allocator_free`,
// `buffer_copy`) calling `malloc` / `free` / `memcpy`.  These calls survive
// the stdlib header whitelist (unlike the `<cstdio>` calls in the other
// fixtures) so they reach checkContainment, where correct caller attribution
// via documentSymbol is required for the external boundary delegation to
// fire.  Without the fix, the synthetic `<l2:...>` caller never matches any
// external name and the run reports false-positive violations.
TEST_F(CppContainmentL2, ContainmentL2MemoryExternalOk) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_memory_external_ok");
    cfg.checkName = "containment";
    cfg.deepMode = true;
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

// --- Allman brace style ---

TEST(CppContainment, ContainmentAllmanStyle) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_allman_style");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

// --- Bypass vector fixes ---

TEST(CppContainment, ContainmentTemplateArgBypass) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_template_arg_bypass");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

TEST(CppContainment, ContainmentStringDispatchBypass) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_string_dispatch_bypass");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}

// --- L1 false positive regression test ---

TEST(CppContainment, ContainmentQualifiedSafe) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_qualified_safe");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
    // Verify no false positives for user-namespaced read/write/open/close
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].second.passed());
    EXPECT_EQ(results[0].second.errorCount, 0);
}

// --- Diagnostic content assertions ---

TEST(CppContainment, ContainmentSystemCallDiagnosticContent) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_system_call");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].second.errorCount, 1);
    bool foundSystem = false;
    for (auto& d : results[0].second.diagnostics) {
        if (d.message.find("system") != std::string::npos) foundSystem = true;
    }
    EXPECT_TRUE(foundSystem) << "Expected system() call to be flagged";
}

// --- Memory capability tests ---

TEST(CppContainment, ContainmentMemoryMalloc) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_memory_malloc");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].second.errorCount, 1);
    bool foundMalloc = false;
    for (auto& d : results[0].second.diagnostics) {
        if (d.message.find("malloc") != std::string::npos ||
            d.message.find("free") != std::string::npos) {
            foundMalloc = true;
        }
    }
    EXPECT_TRUE(foundMalloc) << "Expected malloc/free to be flagged";
}

TEST(CppContainment, ContainmentMemoryMemcpy) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_memory_memcpy");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].second.errorCount, 1);
    bool foundMemcpy = false;
    for (auto& d : results[0].second.diagnostics) {
        if (d.message.find("memcpy") != std::string::npos) {
            foundMemcpy = true;
        }
    }
    EXPECT_TRUE(foundMemcpy) << "Expected memcpy to be flagged";
}

TEST(CppContainment, ContainmentMemoryStrcpy) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_memory_strcpy");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].second.errorCount, 1);
    bool foundStrcpy = false;
    for (auto& d : results[0].second.diagnostics) {
        if (d.message.find("strcpy") != std::string::npos ||
            d.message.find("strcat") != std::string::npos) {
            foundStrcpy = true;
        }
    }
    EXPECT_TRUE(foundStrcpy) << "Expected strcpy/strcat to be flagged";
}

TEST(CppContainment, ContainmentMemoryConstructAt) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_memory_construct_at");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].second.errorCount, 1);
    bool foundConstructAt = false;
    for (auto& d : results[0].second.diagnostics) {
        if (d.message.find("construct_at") != std::string::npos) {
            foundConstructAt = true;
        }
    }
    EXPECT_TRUE(foundConstructAt) << "Expected construct_at to be flagged";
}

TEST(CppContainment, ContainmentMemoryExternalOk) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("containment_memory_external_ok");
    cfg.checkName = "containment";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}
