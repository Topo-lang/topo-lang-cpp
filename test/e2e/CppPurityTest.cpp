// E2E tests for C++ purity checks.
//
// Exercises the full topo-check pipeline for parallel-stage purity:
//   .topo parsing → symbol table → file scan → CppSymbolAccessExtractor
//   → checkPurity → diagnostics.

#include "CheckRunner.h"

#include <gtest/gtest.h>

using namespace topo;

static std::string fixtureDir(const char* name) {
    return std::string(TOPO_TEST_FIXTURES_DIR) + "/" + name;
}

TEST(CppPurity, Pass01_NoGlobalWrites) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("purity_cpp_pass_01");
    cfg.checkName = "purity";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

TEST(CppPurity, Pass02_LocalsOnly) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("purity_cpp_pass_02");
    cfg.checkName = "purity";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

TEST(CppPurity, Pass03_SequentialStagesAllowGlobalWrites) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("purity_cpp_pass_03");
    cfg.checkName = "purity";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

TEST(CppPurity, Pass04_ModeOffSuppressesViolations) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("purity_cpp_pass_04");
    cfg.checkName = "purity";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

TEST(CppPurity, Fail01_ParallelStaticGlobalWrite) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("purity_cpp_fail_01");
    cfg.checkName = "purity";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].second.errorCount, 1);
    bool foundCounter = false;
    for (const auto& d : results[0].second.diagnostics) {
        if (d.message.find("counter") != std::string::npos) foundCounter = true;
    }
    EXPECT_TRUE(foundCounter) << "Expected `counter` to appear in the violation message";
}

TEST(CppPurity, Fail02_ParallelExternGlobalWrite) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("purity_cpp_fail_02");
    cfg.checkName = "purity";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].second.errorCount, 1);
}

TEST(CppPurity, Fail03_IncrementDecrement) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("purity_cpp_fail_03");
    cfg.checkName = "purity";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    // ++ticks and ticks++ are both writes — expect both violations.
    EXPECT_GE(results[0].second.errorCount, 2);
}

TEST(CppPurity, Fail04_MultipleParallelViolations) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("purity_cpp_fail_04");
    cfg.checkName = "purity";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    // Three parallel functions each write a distinct global.
    EXPECT_GE(results[0].second.errorCount, 3);
}
