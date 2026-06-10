// Unit tests for CppAnalysisProvider::collectSourceFiles — source discovery
// over the project dir + the configured source/include entries.
//
// Regression focus: a configured entry that is a plain FILE ("main.cpp" in
// [build].sources reaches collectSourceFiles verbatim) used to be fed to
// fs::recursive_directory_iterator, whose "Not a directory" filesystem_error
// aborted the whole checker (rc=134). File entries must be analyzed as
// single TUs and directory iteration must degrade, not throw.

#include "analysis/CppAnalysisProvider.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

class CppAnalysisProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        projectDir_ = fs::temp_directory_path() /
            ("topo_provider_test_" + std::to_string(topo_getpid()));
        fs::create_directories(projectDir_ / "src");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(projectDir_, ec);
    }

    std::string writeFile(const fs::path& rel, const std::string& content) {
        auto path = projectDir_ / rel;
        std::ofstream ofs(path);
        ofs << content;
        return path.string();
    }

    static int countOf(const std::vector<std::string>& files, const std::string& path) {
        return static_cast<int>(std::count(files.begin(), files.end(), path));
    }

    fs::path projectDir_;
};

TEST_F(CppAnalysisProviderTest, FileSourceEntryIsAnalyzedAsSingleTU) {
    auto mainCpp = writeFile("main.cpp", "int main() { return 0; }\n");
    auto utilCpp = writeFile(fs::path("src") / "util.cpp", "int util() { return 1; }\n");

    auto provider = createCppAnalysisProvider();
    // Simulates [build].sources = ["main.cpp"]: CheckRunner forwards source
    // entries verbatim, so a plain file path arrives as a search entry.
    auto files = provider->collectSourceFiles(projectDir_.string(), {mainCpp});

    // The file entry is picked up as a TU — and only once, even though the
    // projectDir scan discovers the same file (dedup via canonical-ish path).
    EXPECT_EQ(countOf(files, mainCpp), 1);
    EXPECT_EQ(countOf(files, utilCpp), 1);
}

TEST_F(CppAnalysisProviderTest, NonSourceFileEntryIsIgnored) {
    writeFile("main.cpp", "int main() { return 0; }\n");
    auto readme = writeFile("README.md", "# not a TU\n");

    auto provider = createCppAnalysisProvider();
    auto files = provider->collectSourceFiles(projectDir_.string(), {readme});

    EXPECT_EQ(countOf(files, readme), 0);
}

TEST_F(CppAnalysisProviderTest, MissingEntryDegradesToSkip) {
    auto mainCpp = writeFile("main.cpp", "int main() { return 0; }\n");
    auto ghost = (projectDir_ / "no_such_dir").string();

    auto provider = createCppAnalysisProvider();
    auto files = provider->collectSourceFiles(projectDir_.string(), {ghost});

    EXPECT_EQ(countOf(files, mainCpp), 1);
}
