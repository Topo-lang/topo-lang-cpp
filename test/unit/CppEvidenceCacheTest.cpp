#include "CppDriver.h"
#include "NinjaGen.h"
#include "topo/Build/ExecutionEvidence.h"
#include "topo/Distribution/Sha256.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace {
namespace fs = std::filesystem;
namespace ev = topo::build::evidence;
using Clock = std::chrono::steady_clock;

class CppEvidenceCacheTest : public ::testing::Test {
protected:
    fs::path dir, cacheDir;
    topo::build::BuildConfig cfg;
    void SetUp() override {
        dir = fs::temp_directory_path() / ("topo-cpp-cache-" +
            std::to_string(Clock::now().time_since_epoch().count()));
        cacheDir = dir / ".topo-cache";
        ASSERT_TRUE(fs::create_directories(cacheDir / "ir"));
        std::ofstream(dir / "source.cpp") << "int cached_value() { return 1; }\n";
        cfg.sources = {(dir / "source.cpp").string()};
        cfg.buildMode = topo::BuildMode::Aggressive;
        if (const char* compiler = std::getenv("TOPO_TEST_ISOLATED_CLANG")) cfg.hostCompilerPath = compiler;
    }
    void TearDown() override { std::error_code ec; fs::remove_all(dir, ec); }
    static long elapsedMs(Clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }
    std::string bytes(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }
};

TEST_F(CppEvidenceCacheTest, OrdinaryNinjaWarmHitAndSourceInvalidationRemainFunctional) {
    if (!std::getenv("TOPO_TEST_ISOLATED_CLANG")) GTEST_SKIP() << "select a trusted clang for real cache execution";
    if (!topo::build::NinjaGen::isAvailable()) GTEST_SKIP() << "Ninja is unavailable";
    auto start = Clock::now();
    const auto cold = topo::build::compileCppIncremental(cfg, cacheDir);
    RecordProperty("cold_ms", elapsedMs(start));
    ASSERT_EQ(cold.exitCode, 0); ASSERT_EQ(cold.outputFiles.size(), 1u);
    const auto output = fs::path(cold.outputFiles.front());
    const auto originalMtime = fs::last_write_time(output);
    const auto originalBytes = bytes(output);
    ASSERT_NE(originalBytes.find("ret i32 1"), std::string::npos);

    start = Clock::now();
    const auto warm = topo::build::compileCppIncremental(cfg, cacheDir);
    RecordProperty("warm_ms", elapsedMs(start));
    ASSERT_EQ(warm.exitCode, 0);
    EXPECT_EQ(fs::last_write_time(output), originalMtime);
    EXPECT_EQ(bytes(output), originalBytes);

    std::ofstream(cfg.sources.front()) << "int cached_value() { return 2; }\n";
    fs::last_write_time(cfg.sources.front(), originalMtime + std::chrono::seconds(1));
    start = Clock::now();
    const auto invalidated = topo::build::compileCppIncremental(cfg, cacheDir);
    RecordProperty("invalidated_ms", elapsedMs(start));
    ASSERT_EQ(invalidated.exitCode, 0);
    EXPECT_NE(bytes(output), originalBytes);
    EXPECT_NE(bytes(output).find("ret i32 2"), std::string::npos);
}

TEST_F(CppEvidenceCacheTest, EvidenceRequestAndRecorderEachForceActualCompilation) {
    if (!std::getenv("TOPO_TEST_ISOLATED_CLANG")) GTEST_SKIP() << "select a trusted clang for real cache execution";
    if (!topo::build::NinjaGen::isAvailable()) GTEST_SKIP() << "Ninja is unavailable";
    const auto ordinary = topo::build::compileCppIncremental(cfg, cacheDir);
    ASSERT_EQ(ordinary.exitCode, 0); ASSERT_EQ(ordinary.outputFiles.size(), 1u);

    cfg.evidenceMode = "commands";
    auto start = Clock::now();
    const auto strict = topo::build::compileCppIncremental(cfg, cacheDir);
    RecordProperty("strict_cold_ms", elapsedMs(start));
    ASSERT_EQ(strict.exitCode, 0); ASSERT_EQ(strict.outputFiles.size(), 1u);
    EXPECT_NE(strict.outputFiles.front(), ordinary.outputFiles.front());
    // A distinct fixed timestamp makes compiler execution observable without
    // interpreting logs or relying on timestamp resolution between calls.
    fs::last_write_time(strict.outputFiles.front(), fs::file_time_type::clock::now() + std::chrono::hours(1));
    const auto sentinelTime = fs::last_write_time(strict.outputFiles.front());

    cfg.evidenceMode.clear();
    ev::ExecutionRecorder recorder({{"project", dir}}, dir / "evidence.json", "cache-rebuild-test");
    start = Clock::now();
    const auto repeated = topo::build::compileCppIncremental(cfg, cacheDir, &recorder);
    RecordProperty("strict_repeated_ms", elapsedMs(start));
    ASSERT_EQ(repeated.exitCode, 0);
    EXPECT_NE(fs::last_write_time(strict.outputFiles.front()), sentinelTime);
    const auto manifest = recorder.manifest(); ASSERT_EQ(manifest.nodes.size(), 1u);
    EXPECT_EQ(manifest.nodes.front().action.kind, "cpp-compile");
    EXPECT_FALSE(manifest.nodes.front().complete);
    EXPECT_EQ(manifest.nodes.front().producer.at("builder"), "cache-rebuild-test");
    EXPECT_TRUE(recorder.diagnostics().empty());
    const auto verified = ev::verifyManifest(manifest, {{"project", dir}});
    EXPECT_TRUE(verified.integrity);
    EXPECT_FALSE(verified.complete);
    EXPECT_FALSE(verified.buildEvidence);
}

TEST_F(CppEvidenceCacheTest, IncrementalEntryPointRejectsForgedCheckedModelBeforeCompiling) {
    if (!std::getenv("TOPO_TEST_ISOLATED_CLANG")) GTEST_SKIP() << "select a trusted clang for check-model validation";
    cfg.checkModelPath = (dir / "compile_commands.json").string();
    std::string error;
    ASSERT_TRUE(topo::build::writeCppCheckModel(cfg, cacheDir / "ir", cfg.checkModelPath, error)) << error;
    auto model = nlohmann::json::parse(bytes(cfg.checkModelPath));
    model[0]["arguments"].insert(model[0]["arguments"].begin() + 1, "-DHIDDEN=1");
    std::ofstream(cfg.checkModelPath) << model.dump(2) << "\n";
    cfg.checkModelSha256 = topo::dist::sha256File(cfg.checkModelPath);
    const auto result = topo::build::compileCppIncremental(cfg, cacheDir);
    EXPECT_NE(result.exitCode, 0);
    EXPECT_TRUE(fs::is_empty(cacheDir / "ir"));
    EXPECT_FALSE(fs::exists(cacheDir / "build.ninja"));
}

TEST_F(CppEvidenceCacheTest, IsolatedEvidenceRequiresTheDedicatedBuilder) {
    cfg.evidenceMode = "isolated-build";
    EXPECT_NE(topo::build::compileCpp(cfg, cacheDir / "ir").exitCode, 0);
    EXPECT_NE(topo::build::compileCppIncremental(cfg, cacheDir).exitCode, 0);
    EXPECT_TRUE(fs::is_empty(cacheDir / "ir"));
}
} // namespace
