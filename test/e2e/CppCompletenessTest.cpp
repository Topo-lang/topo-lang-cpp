// E2E tests for C++ completeness checks.

#include "CheckRunner.h"

#include <gtest/gtest.h>

using namespace topo;

static std::string fixtureDir(const char* name) {
    return std::string(TOPO_TEST_FIXTURES_DIR) + "/" + name;
}

TEST(CppCompleteness, CompletenessPassFixture) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("completeness_pass");
    cfg.checkName = "completeness";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 0);
}

TEST(CppCompleteness, CompletenessFailFixture) {
    CheckConfig cfg;
    cfg.projectDir = fixtureDir("completeness_fail");
    cfg.checkName = "completeness";
    CheckRunner runner(cfg);
    ASSERT_TRUE(runner.loadConfig());
    EXPECT_EQ(runner.run(), 1);
}
