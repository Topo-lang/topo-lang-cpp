#include "CppDriver.h"
#include "topo/Build/ExecutionEvidence.h"
#include "topo/Distribution/Sha256.h"
#include "topo/Platform/ToolResolution.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace {
namespace fs=std::filesystem;
class CppCommandModelTest : public ::testing::Test {
protected:
    fs::path dir;
    topo::build::BuildConfig cfg;
    void SetUp() override {
        dir=fs::temp_directory_path()/("topo-cpp-model-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directory(dir);fs::create_directory(dir/"first");fs::create_directory(dir/"second");fs::create_directory(dir/"ir");
        std::ofstream(dir/"first/unit.cpp")<<"int first() { return 1; }\n";
        std::ofstream(dir/"second/unit.cpp")<<"int second() { return 2; }\n";
        cfg.sources={(dir/"first/unit.cpp").string(),(dir/"second/unit.cpp").string()};
        cfg.evidenceMode="commands";
        cfg.hostCompilerPath=topo::platform::findSpawnableOnPath("cmake"); // model-only tests never execute this tool
        ASSERT_FALSE(cfg.hostCompilerPath.empty());
        if(const char* compiler=std::getenv("TOPO_TEST_ISOLATED_CLANG"))cfg.hostCompilerPath=compiler;
    }
    void TearDown() override {std::error_code ec;fs::remove_all(dir,ec);}
    void freezeModel() {
        cfg.checkModelPath=(dir/"compile_commands.json").string();std::string error;
        ASSERT_TRUE(topo::build::writeCppCheckModel(cfg,dir/"ir",cfg.checkModelPath,error))<<error;
        cfg.checkModelSha256=topo::dist::sha256File(cfg.checkModelPath);
    }
    nlohmann::json model() {std::ifstream input(cfg.checkModelPath);nlohmann::json value;input>>value;return value;}
};
TEST_F(CppCommandModelTest, SameBasenamesHaveDistinctActualOutputsAndOrderedFlags) {
    cfg.cppFlags={"-DORDER=1","-DORDER=2"};freezeModel();auto value=model();ASSERT_EQ(value.size(),2u);
    EXPECT_NE(value[0].at("arguments").back(),value[1].at("arguments").back());
    auto args=value[0].at("arguments").get<std::vector<std::string>>();
    auto one=std::find(args.begin(),args.end(),"-DORDER=1"),two=std::find(args.begin(),args.end(),"-DORDER=2");
    ASSERT_NE(one,args.end());ASSERT_NE(two,args.end());EXPECT_LT(one,two);
    std::string error;EXPECT_TRUE(topo::build::validateCppCheckModel(cfg,dir/"ir",error))<<error;
}
TEST_F(CppCommandModelTest, SourceChangesKeepingMtimeInvalidateCheckedModel) {
    freezeModel();const auto modified=fs::last_write_time(cfg.sources.front());
    std::ofstream(cfg.sources.front())<<"int first() { return 9; }\n";fs::last_write_time(cfg.sources.front(),modified);
    std::string error;EXPECT_FALSE(topo::build::validateCppCheckModel(cfg,dir/"ir",error));
    EXPECT_NE(error.find("differs"),std::string::npos);
}
TEST_F(CppCommandModelTest, RehashedForgedCommandCannotReplaceFinalBuildConfiguration) {
    freezeModel();auto forged=model();forged[0]["arguments"].insert(forged[0]["arguments"].begin()+1,"-DHIDDEN=1");
    std::ofstream(cfg.checkModelPath)<<forged.dump(2)<<"\n";
    cfg.checkModelSha256=topo::dist::sha256File(cfg.checkModelPath);
    std::string error;EXPECT_FALSE(topo::build::validateCppCheckModel(cfg,dir/"ir",error));
    EXPECT_NE(error.find("differs"),std::string::npos);
}
TEST_F(CppCommandModelTest, ActualCompilerExecutesTheCheckedArgumentVectors) {
    if(!std::getenv("TOPO_TEST_ISOLATED_CLANG"))GTEST_SKIP()<<"set a trusted clang for actual direct-compile validation";
    freezeModel();const auto expected=model();
    topo::build::evidence::ExecutionRecorder recorder({{"project",dir}},dir/"commands.json","test");
    const auto result=topo::build::compileCpp(cfg,dir/"ir",&recorder);
    ASSERT_EQ(result.exitCode,0);ASSERT_EQ(result.outputFiles.size(),2u);
    auto manifest=recorder.manifest();ASSERT_EQ(manifest.nodes.size(),2u);
    for(const auto& record:manifest.nodes){
        bool matched=false;for(const auto& command:expected){
            auto args=command.at("arguments").get<std::vector<std::string>>();args.erase(args.begin());
            if(args==record.action.argv)matched=true;
        }
        EXPECT_TRUE(matched);EXPECT_FALSE(record.outputs.empty());
    }
    EXPECT_TRUE(recorder.diagnostics().empty());
    EXPECT_TRUE(topo::build::evidence::verifyManifest(manifest,{{"project",dir}}).integrity);
}
}
