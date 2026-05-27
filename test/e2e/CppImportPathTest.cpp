// E2E tests for C++ import-path checks.

#include "CheckRunner.h"

#include <gtest/gtest.h>

using namespace topo;

static std::string fixtureDir(const char* name) {
    return std::string(TOPO_TEST_FIXTURES_DIR) + "/" + name;
}

TEST(CppImportPath, ImportPathPassFixture) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("import_path_pass");
    cfg.checkName = "import-path";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, "import-path");
    EXPECT_TRUE(results[0].second.passed());
}

TEST(CppImportPath, ImportPathFailFixture) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("import_path_fail");
    cfg.checkName = "import-path";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
    auto& results = runner.lastResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].second.errorCount, 1);
    bool foundHeader = false;
    for (auto& d : results[0].second.diagnostics) {
        if (d.message.find("nonexistent_header_xyz.h") != std::string::npos)
            foundHeader = true;
    }
    EXPECT_TRUE(foundHeader) << "Expected nonexistent header to be flagged";
}
