#include "IsolatedCppBuild.h"
#include "topo/Build/ArtifactEvidence.h"
#include "topo/Distribution/Sha256.h"
#include "topo/Platform/Process.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace {
namespace fs=std::filesystem;
class IsolatedCppBuildTest : public ::testing::Test {
protected:
    fs::path dir;
    topo::build::BuildConfig cfg;
    void SetUp() override {
        dir=fs::temp_directory_path()/("topo-isolated-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directory(dir);
        std::ofstream(dir/"add.cpp") << "extern \"C\" int topo_add(int a, int b) { return a + b; }\n";
        cfg.sources={(dir/"add.cpp").string()}; cfg.outputType=topo::OutputType::Shared;
        cfg.outputPath=(dir/"libadd.dylib").string();cfg.evidenceOutput=(dir/"evidence.json").string();
        if(const char* clang=std::getenv("TOPO_TEST_ISOLATED_CLANG")) cfg.hostCompilerPath=clang;
        if(const char* ld=std::getenv("TOPO_TEST_ISOLATED_LD")) cfg.evidenceLinkerPath=ld;
        cfg.evidenceCompilerSha256=topo::dist::sha256File(cfg.hostCompilerPath);
        cfg.evidenceLinkerSha256=topo::dist::sha256File(cfg.evidenceLinkerPath);
    }
    void TearDown() override {
        std::error_code ec;
        for(fs::recursive_directory_iterator i(dir,ec),end;!ec&&i!=end;i.increment(ec))
            if(i->is_directory())fs::permissions(i->path(),fs::perms::owner_all,ec);
        fs::remove_all(dir,ec);
    }
    bool configured() const {return !cfg.hostCompilerPath.empty()&&!cfg.evidenceLinkerPath.empty();}
};
TEST_F(IsolatedCppBuildTest, ExtraFlagsFailBeforeCompilerExecution) {
    cfg.cppFlags={"-include", "/unapproved/file"};std::string error;
    EXPECT_NE(topo::build::buildIsolatedCpp(cfg,error).exitCode,0);EXPECT_FALSE(error.empty());
    EXPECT_FALSE(fs::exists(cfg.outputPath));
}
TEST_F(IsolatedCppBuildTest, WrongCompilerIdentityFailsBeforeBuild) {
    cfg.evidenceCompilerSha256=std::string(64,'0');std::string error;
    EXPECT_NE(topo::build::buildIsolatedCpp(cfg,error).exitCode,0);EXPECT_FALSE(error.empty());
    EXPECT_FALSE(fs::exists(cfg.outputPath));
}
TEST_F(IsolatedCppBuildTest, RealSandboxBuildBindsActualExportsAndBytes) {
    if(!configured())GTEST_SKIP()<<"set TOPO_TEST_ISOLATED_CLANG and TOPO_TEST_ISOLATED_LD to trusted macOS ARM64 tools";
    std::string error;const auto original=topo::dist::sha256File(cfg.sources.front());
    auto result=topo::build::buildIsolatedCpp(cfg,error);ASSERT_EQ(result.exitCode,0)<<error;
    EXPECT_EQ(topo::dist::sha256File(cfg.sources.front()),original);
    topo::build::evidence::Manifest manifest;
    ASSERT_TRUE(topo::build::evidence::readManifest(cfg.evidenceOutput,manifest,error))<<error;
    ASSERT_EQ(manifest.nodes.size(),1u);const auto& record=manifest.nodes.front();
    EXPECT_FALSE(record.complete);EXPECT_EQ(record.producer.at("isolation"),"macos-sandbox-exec");
    auto verification=topo::build::evidence::verifyManifest(manifest,{{"build",record.producer.at("buildRoot")},{"artifact",record.producer.at("artifactRoot")}});
    EXPECT_TRUE(verification.integrity);EXPECT_FALSE(verification.buildEvidence);
    std::ifstream abiFile(cfg.outputPath+".topo-abi.json");nlohmann::json abi;abiFile>>abi;
    ASSERT_EQ(abi.at("exports").size(),1u);EXPECT_EQ(abi.at("exports")[0].at("signature"),"int (int, int)");
    EXPECT_EQ(abi.at("artifact").at("sha256"),topo::dist::sha256File(cfg.outputPath));
}
TEST_F(IsolatedCppBuildTest, NativeCallsOutsideScalarProfileAreRejected) {
    if(!configured())GTEST_SKIP()<<"set trusted isolated macOS tool paths";
    std::ofstream(dir/"add.cpp")<<"extern \"C\" int puts(const char*); extern \"C\" int topo_add(int a,int b){puts(\"unapproved\");return a+b;}\n";
    std::string error;EXPECT_NE(topo::build::buildIsolatedCpp(cfg,error).exitCode,0);EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("only defined extern C scalar"),std::string::npos)<<error;
    EXPECT_FALSE(fs::exists(cfg.outputPath));
}
TEST_F(IsolatedCppBuildTest, FreshBuildDirectoriesReproduceTheSamePayload) {
    if(!configured())GTEST_SKIP()<<"set trusted isolated macOS tool paths";
    std::string error;
    ASSERT_EQ(topo::build::buildIsolatedCpp(cfg,error).exitCode,0)<<error;
    const auto firstDigest=topo::dist::sha256File(cfg.outputPath);
    const auto second=dir/"independent-build";
    ASSERT_TRUE(fs::create_directory(second));
    fs::copy_file(cfg.sources.front(),second/"add.cpp");
    auto rebuilt=cfg;
    rebuilt.sources={(second/"add.cpp").string()};
    rebuilt.outputPath=(second/"libadd.dylib").string();
    rebuilt.evidenceOutput=(second/"evidence.json").string();
    ASSERT_EQ(topo::build::buildIsolatedCpp(rebuilt,error).exitCode,0)<<error;
    EXPECT_EQ(topo::dist::sha256File(rebuilt.outputPath),firstDigest);
    topo::build::evidence::Manifest first,other;
    ASSERT_TRUE(topo::build::evidence::readManifest(cfg.evidenceOutput,first,error))<<error;
    ASSERT_TRUE(topo::build::evidence::readManifest(rebuilt.evidenceOutput,other,error))<<error;
    ASSERT_EQ(first.nodes.size(),1u);ASSERT_EQ(other.nodes.size(),1u);
    EXPECT_NE(first.nodes[0].producer.at("buildRoot"),other.nodes[0].producer.at("buildRoot"));
    EXPECT_FALSE(first.nodes[0].complete);EXPECT_FALSE(other.nodes[0].complete);
    RecordProperty("payloadSha256",firstDigest);
}
TEST_F(IsolatedCppBuildTest, InlineAssemblyIsRejectedBeforeLink) {
    if(!configured())GTEST_SKIP()<<"set trusted isolated macOS tool paths";
    std::ofstream(dir/"add.cpp")<<"extern \"C\" int topo_add(int a,int b){asm(\"nop\");return a+b;}\n";
    std::string error;EXPECT_NE(topo::build::buildIsolatedCpp(cfg,error).exitCode,0);EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("only defined extern C scalar"),std::string::npos)<<error;
    EXPECT_FALSE(fs::exists(cfg.outputPath));
}
TEST_F(IsolatedCppBuildTest, KernelDeniesWritesAndNetworkWithRunnableControlProbe) {
    if(!configured())GTEST_SKIP()<<"set trusted isolated macOS tool paths";
    std::string probe;
#ifdef TOPO_SANDBOX_PROBE_BINARY
    probe=TOPO_SANDBOX_PROBE_BINARY;
#endif
    if(const char* value=std::getenv("TOPO_TEST_SANDBOX_PROBE"))probe=value;
    if(probe.empty())GTEST_SKIP()<<"sandbox probe binary is unavailable on this platform";
    std::string error;ASSERT_EQ(topo::build::buildIsolatedCpp(cfg,error).exitCode,0)<<error;
    topo::build::evidence::Manifest manifest;
    ASSERT_TRUE(topo::build::evidence::readManifest(cfg.evidenceOutput,manifest,error))<<error;
    const fs::path stage=manifest.nodes.front().producer.at("buildRoot");
    const fs::path sentinel=stage/"writable-sentinel";
    std::ofstream(sentinel)<<"unchanged";fs::permissions(sentinel,fs::perms::owner_read|fs::perms::owner_write);
    const auto digest=topo::dist::sha256File(sentinel.string());
    auto control=topo::platform::runProcessCapture(probe,{sentinel.string()});
    EXPECT_EQ(control.exitCode,1);EXPECT_NE(control.stdoutOutput.find("writeDenied=0"),std::string::npos);
    std::ifstream profileFile(stage/"sandbox.sb");
    std::string profile{std::istreambuf_iterator<char>(profileFile),{}};
    profile+="\n(allow file-read* (literal "+nlohmann::json(fs::canonical(probe).string()).dump()+"))\n";
    const auto testProfile=dir/"probe.sb";std::ofstream(testProfile)<<profile;
    auto protectedRun=topo::platform::runProcessCaptureWithTimeout("/usr/bin/sandbox-exec",
        {"-f",testProfile.string(),probe,sentinel.string()},10000);
    ASSERT_EQ(protectedRun.exitCode,0)<<protectedRun.stderrOutput<<protectedRun.stdoutOutput;
    EXPECT_NE(protectedRun.stdoutOutput.find("writeDenied=1"),std::string::npos);
    EXPECT_NE(protectedRun.stdoutOutput.find("networkDenied=1"),std::string::npos);
    EXPECT_EQ(topo::dist::sha256File(sentinel.string()),digest);
}
}
