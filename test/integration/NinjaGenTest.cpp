#include "NinjaGen.h"
#include "topo/Build/BuildConfig.h"

using namespace topo;

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace topo::build;

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

class NinjaGenTest : public ::testing::Test {
protected:
    fs::path testDir;
    fs::path cacheDir;
    fs::path irDir;

    void SetUp() override {
        testDir = fs::temp_directory_path() / ("topo-ninja-test_" + std::to_string(topo_getpid()));
        cacheDir = testDir / ".topo-cache";
        irDir = cacheDir / "ir";
        fs::create_directories(irDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(testDir, ec);
    }

    std::string readFile(const fs::path& path) {
        std::ifstream ifs(path);
        std::ostringstream oss;
        oss << ifs.rdbuf();
        return oss.str();
    }
};

// --- flattenSourceName ---

TEST_F(NinjaGenTest, FlattenSimpleName) {
    auto result = NinjaGen::flattenSourceName("src/main.cpp", testDir);
    EXPECT_EQ(result, "src_main");
}

TEST_F(NinjaGenTest, FlattenNestedPath) {
    // Use a path inside testDir so fs::relative works correctly
    fs::path src = testDir / "lib" / "core" / "engine.cpp";
    auto result = NinjaGen::flattenSourceName(src.string(), testDir);
    EXPECT_EQ(result, "lib_core_engine");
}

TEST_F(NinjaGenTest, FlattenAbsolutePath) {
    // When the source is inside testDir, it should become relative
    fs::path src = testDir / "src" / "main.cpp";
    auto result = NinjaGen::flattenSourceName(src.string(), testDir);
    EXPECT_EQ(result, "src_main");
}

TEST_F(NinjaGenTest, FlattenAvoidsSameNameConflict) {
    // src/main.cpp and lib/main.cpp should produce different names
    auto r1 = NinjaGen::flattenSourceName("src/main.cpp", testDir);
    auto r2 = NinjaGen::flattenSourceName("lib/main.cpp", testDir);
    EXPECT_NE(r1, r2);
}

// --- generate ---

TEST_F(NinjaGenTest, GenerateBasicNinjaFile) {
    BuildConfig cfg;
    cfg.hostCompilerPath = "/usr/bin/clang++";
    cfg.standard = "c++17";
    cfg.sources = {(testDir / "src/main.cpp").string(), (testDir / "src/engine.cpp").string()};
    cfg.outputType = OutputType::Exe;

    fs::path ninjaPath = cacheDir / "build.ninja";
    ASSERT_TRUE(NinjaGen::generate(cfg, ninjaPath, irDir));

    std::string content = readFile(ninjaPath);

    // Check basic structure
    EXPECT_NE(content.find("ninja_required_version = 1.3"), std::string::npos);
    EXPECT_NE(content.find("rule cc"), std::string::npos);
    EXPECT_NE(content.find("-std=c++17"), std::string::npos);
    EXPECT_NE(content.find("-S -emit-llvm -O2"), std::string::npos);
    EXPECT_NE(content.find("-MD -MF"), std::string::npos);
    EXPECT_NE(content.find("deps = gcc"), std::string::npos);
    EXPECT_NE(content.find("depfile = $DEP_FILE"), std::string::npos);

    // Check build edges
    EXPECT_NE(content.find("src_main.ll"), std::string::npos);
    EXPECT_NE(content.find("src_engine.ll"), std::string::npos);
}

TEST_F(NinjaGenTest, GenerateWithIncludes) {
    BuildConfig cfg;
    cfg.hostCompilerPath = "clang++";
    cfg.standard = "c++20";
    cfg.sources = {(testDir / "main.cpp").string()};
    cfg.includeDirs = {"/usr/include/mylib", "/opt/boost/include"};
    cfg.outputType = OutputType::Exe;

    fs::path ninjaPath = cacheDir / "build.ninja";
    ASSERT_TRUE(NinjaGen::generate(cfg, ninjaPath, irDir));

    std::string content = readFile(ninjaPath);
    EXPECT_NE(content.find("-I/usr/include/mylib"), std::string::npos);
    EXPECT_NE(content.find("-I/opt/boost/include"), std::string::npos);
    EXPECT_NE(content.find("-std=c++20"), std::string::npos);
}

TEST_F(NinjaGenTest, GenerateWithDefines) {
    BuildConfig cfg;
    cfg.hostCompilerPath = "clang++";
    cfg.standard = "c++17";
    cfg.sources = {(testDir / "main.cpp").string()};
    cfg.embedIR = true;
    cfg.adaptiveCfg.mode = FeatureMode::Auto;
    cfg.outputType = OutputType::Exe;

    fs::path ninjaPath = cacheDir / "build.ninja";
    ASSERT_TRUE(NinjaGen::generate(cfg, ninjaPath, irDir));

    std::string content = readFile(ninjaPath);
    EXPECT_NE(content.find("-DTOPO_HAS_JIT"), std::string::npos);
    EXPECT_NE(content.find("-DTOPO_HAS_ADAPTIVE"), std::string::npos);
}

#ifndef _WIN32
TEST_F(NinjaGenTest, GenerateSharedLibFlagsUnix) {
    BuildConfig cfg;
    cfg.hostCompilerPath = "clang++";
    cfg.standard = "c++17";
    cfg.sources = {(testDir / "main.cpp").string()};
    cfg.outputType = OutputType::Shared;

    fs::path ninjaPath = cacheDir / "build.ninja";
    ASSERT_TRUE(NinjaGen::generate(cfg, ninjaPath, irDir));

    std::string content = readFile(ninjaPath);
    EXPECT_NE(content.find("-fPIC"), std::string::npos);
    EXPECT_NE(content.find("-fvisibility=hidden"), std::string::npos);
}
#endif

// --- isAvailable / resolveNinja ---

TEST_F(NinjaGenTest, ResolveNinjaReturnsString) {
    // Just verify it returns a string (may be empty if ninja not installed)
    std::string result = NinjaGen::resolveNinja();
    // No assertion on the value - depends on the environment
    (void)result;
}
