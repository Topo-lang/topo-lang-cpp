#include "../../topo-check/extractor/CppAuditNativeFile.h"

#include <gtest/gtest.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {
namespace fs = std::filesystem;
using topo::check::CppAuditReadPolicy;
using topo::check::readCppAuditNativeDirectory;
using topo::check::readCppAuditNativeFile;

class CppAuditNativeFileTest : public ::testing::Test {
protected:
    static constexpr size_t kPathLimit = 16 * 1024;
    fs::path root;
    CppAuditReadPolicy policy;

    void SetUp() override {
        llvm::SmallString<128> temporary;
        const auto ec = llvm::sys::fs::createUniqueDirectory("topo-native-file", temporary);
        ASSERT_FALSE(ec) << ec.message();
        std::error_code canonicalError;
        root = fs::canonical(fs::path(temporary.str().str()), canonicalError);
        ASSERT_FALSE(canonicalError) << canonicalError.message();
        policy.roots.push_back(root.generic_string());
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path write(const std::string& name, const std::string& content) {
        const auto path = root / name;
        std::ofstream output(path, std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        EXPECT_TRUE(output.good());
        return path;
    }
};

TEST_F(CppAuditNativeFileTest, EmptyFileAcceptsZeroContentBudget) {
    const auto file = write("empty", "");
    auto result = readCppAuditNativeFile(file.string(), policy, 0, kPathLimit);
    ASSERT_TRUE(result) << result.getError().message();
    ASSERT_TRUE(result->bytes);
    EXPECT_TRUE(result->bytes->empty());
    EXPECT_EQ(result->status.getSize(), 0u);
    EXPECT_EQ(result->status.getName(), file.string());
    EXPECT_TRUE(result->status.isRegularFile());
}

TEST_F(CppAuditNativeFileTest, PreservesBinaryBytesIncludingCrLfAndControlZ) {
    const std::string bytes("a\r\nb\x1a\0z", 7);
    const auto file = write("binary", bytes);
    auto result = readCppAuditNativeFile(file.string(), policy, bytes.size(), kPathLimit);
    ASSERT_TRUE(result) << result.getError().message();
    EXPECT_EQ(*result->bytes, bytes);
    EXPECT_EQ(result->status.getSize(), bytes.size());
}

TEST_F(CppAuditNativeFileTest, RejectsUnauthorizedRegularFile) {
    const auto file = write("secret", "secret");
    auto result = readCppAuditNativeFile(file.string(), {}, 64, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::permission_denied);
}

TEST_F(CppAuditNativeFileTest, ExactFileGrantDoesNotGrantItsSibling) {
    const auto allowed = write("allowed", "yes");
    const auto sibling = write("sibling", "no");
    CppAuditReadPolicy exact;
    exact.files.push_back(allowed.generic_string());
    auto yes = readCppAuditNativeFile(allowed.string(), exact, 16, kPathLimit);
    ASSERT_TRUE(yes) << yes.getError().message();
    auto no = readCppAuditNativeFile(sibling.string(), exact, 16, kPathLimit);
    ASSERT_FALSE(no);
    EXPECT_EQ(no.getError(), std::errc::permission_denied);
}

TEST_F(CppAuditNativeFileTest, RootGrantRequiresAComponentBoundary) {
    fs::create_directory(root / "allowed");
    fs::create_directory(root / "allowed-other");
    const auto file = write("allowed-other/secret", "secret");
    CppAuditReadPolicy narrow;
    narrow.roots.push_back((root / "allowed").generic_string());
    auto result = readCppAuditNativeFile(file.string(), narrow, 64, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::permission_denied);
}

TEST_F(CppAuditNativeFileTest, EnforcesContentBudgetBeforeReading) {
    const auto file = write("content", "0123456789");
    auto result = readCppAuditNativeFile(file.string(), policy, 9, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::file_too_large);
    auto exact = readCppAuditNativeFile(file.string(), policy, 10, kPathLimit);
    ASSERT_TRUE(exact) << exact.getError().message();
    EXPECT_EQ(*exact->bytes, "0123456789");
}

TEST_F(CppAuditNativeFileTest, RejectsRelativeNullAndOverlongSpellings) {
    auto relative = readCppAuditNativeFile("relative", policy, 16, kPathLimit);
    ASSERT_FALSE(relative);
    EXPECT_EQ(relative.getError(), std::errc::invalid_argument);
    const auto file = write("file", "a");
    std::string nul = file.string();
    nul.push_back('\0');
    nul += "suffix";
    auto embedded = readCppAuditNativeFile(nul, policy, 16, kPathLimit);
    ASSERT_FALSE(embedded);
    EXPECT_EQ(embedded.getError(), std::errc::invalid_argument);
    auto longPath = readCppAuditNativeFile(file.string(), policy, 16, 1);
    ASSERT_FALSE(longPath);
    EXPECT_EQ(longPath.getError(), std::errc::filename_too_long);
}

TEST_F(CppAuditNativeFileTest, RejectsDirectoryAsFile) {
    auto result = readCppAuditNativeFile(root.string(), policy, 1024, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::is_a_directory);
}

TEST_F(CppAuditNativeFileTest, HardLinksPreserveNativeIdentity) {
    const auto file = write("original", "shared");
    const auto alias = root / "alias";
    std::error_code ec;
    fs::create_hard_link(file, alias, ec);
    ASSERT_FALSE(ec) << ec.message();
    auto first = readCppAuditNativeFile(file.string(), policy, 16, kPathLimit);
    auto second = readCppAuditNativeFile(alias.string(), policy, 16, kPathLimit);
    ASSERT_TRUE(first) << first.getError().message();
    ASSERT_TRUE(second) << second.getError().message();
    EXPECT_EQ(first->status.getUniqueID(), second->status.getUniqueID());
    EXPECT_EQ(*first->bytes, *second->bytes);
    EXPECT_EQ(second->status.getName(), alias.string());
}

#if defined(__APPLE__) || defined(__linux__)

TEST_F(CppAuditNativeFileTest, HardLinkGrantAppliesToTheOpenedPhysicalPath) {
    const auto file = write("original", "shared");
    const auto alias = root / "alias";
    fs::create_hard_link(file, alias);
    CppAuditReadPolicy exact;
    exact.files.push_back(file.generic_string());
    auto result = readCppAuditNativeFile(alias.string(), exact, 16, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::permission_denied);
}

TEST_F(CppAuditNativeFileTest, ResolvesSymlinkBeforeDotDotWithoutLosingSpelling) {
    fs::create_directory(root / "left");
    fs::create_directories(root / "right/nested");
    const auto left = write("left/which.h", "11");
    const auto right = write("right/which.h", "22");
    fs::create_directory_symlink("../right/nested", root / "left/link");
    const std::string spelling = (root / "left/link/../which.h").string();
    auto first = readCppAuditNativeFile(spelling, policy, 16, kPathLimit);
    auto second = readCppAuditNativeFile(left.string(), policy, 16, kPathLimit);
    ASSERT_TRUE(first) << first.getError().message();
    ASSERT_TRUE(second) << second.getError().message();
    EXPECT_EQ(*first->bytes, "22");
    EXPECT_EQ(*second->bytes, "11");
    EXPECT_EQ(first->physicalPath, right.string());
    EXPECT_EQ(first->status.getName(), spelling);
    EXPECT_NE(first->status.getUniqueID(), second->status.getUniqueID());
}

TEST_F(CppAuditNativeFileTest, SymlinkInsideGrantCannotReadOutsideTarget) {
    fs::create_directory(root / "allowed");
    const auto file = write("outside", "secret");
    const auto link = root / "allowed/link";
    fs::create_symlink(file, link);
    CppAuditReadPolicy narrow;
    narrow.roots.push_back((root / "allowed").generic_string());
    auto result = readCppAuditNativeFile(link.string(), narrow, 16, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::permission_denied);
}

TEST_F(CppAuditNativeFileTest, FifoWithoutWriterIsRejectedWithoutBlocking) {
    const auto fifo = root / "fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    const auto start = std::chrono::steady_clock::now();
    auto result = readCppAuditNativeFile(fifo.string(), policy, 16, kPathLimit);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::not_supported);
}

TEST_F(CppAuditNativeFileTest, DirectoryMatchesNativeOrderAndPreservesEntrySpelling) {
    write("alpha", "a");
    write("beta", "b");
    fs::create_directory(root / "child");
    fs::create_symlink("alpha", root / "link");
    std::vector<std::string> names;
    DIR* native = ::opendir(root.c_str());
    ASSERT_NE(native, nullptr);
    while (const auto* entry = ::readdir(native)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..")
            names.push_back(name);
    }
    ::closedir(native);
    const std::string spelling = root.string() + "/.";
    auto result = readCppAuditNativeDirectory(spelling, policy, 16, 4096, kPathLimit);
    ASSERT_TRUE(result) << result.getError().message();
    EXPECT_FALSE(result->tailError);
    EXPECT_TRUE(result->status.isDirectory());
    EXPECT_EQ(result->status.getName(), spelling);
    EXPECT_EQ(result->physicalPath, root.string());
    ASSERT_EQ(result->entries.size(), names.size());
    for (size_t i = 0; i < names.size(); ++i)
        EXPECT_EQ(result->entries[i].path(), spelling + "/" + names[i]);
    for (const auto& entry : result->entries)
        if (entry.path() == spelling + "/link")
            EXPECT_EQ(entry.type(), llvm::sys::fs::file_type::symlink_file);
}

TEST_F(CppAuditNativeFileTest, EmptyDirectoryAcceptsZeroBudgets) {
    auto result = readCppAuditNativeDirectory(root.string(), policy, 0, 0, kPathLimit);
    ASSERT_TRUE(result) << result.getError().message();
    EXPECT_TRUE(result->entries.empty());
    EXPECT_FALSE(result->tailError);
}

TEST_F(CppAuditNativeFileTest, RejectsDirectoryEntryAndStringBudgets) {
    write("one", "1");
    write("two", "2");
    auto entries = readCppAuditNativeDirectory(root.string(), policy, 1, 4096, kPathLimit);
    ASSERT_FALSE(entries);
    EXPECT_EQ(entries.getError(), std::errc::file_too_large);
    auto strings = readCppAuditNativeDirectory(root.string(), policy, 8, 1, kPathLimit);
    ASSERT_FALSE(strings);
    EXPECT_EQ(strings.getError(), std::errc::file_too_large);
}

TEST_F(CppAuditNativeFileTest, DirectoryPathBudgetsZeroAndOneStayBounded) {
    for (size_t limit : {size_t{0}, size_t{1}}) {
        auto result = readCppAuditNativeDirectory(root.string(), policy, 16, 4096, limit);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.getError(), std::errc::filename_too_long);
    }
    CppAuditReadPolicy filesystemRoot;
    filesystemRoot.roots.push_back("/");
    auto rootOnly = readCppAuditNativeDirectory("/", filesystemRoot, 16, 4096, 1);
    ASSERT_FALSE(rootOnly);
    EXPECT_EQ(rootOnly.getError(), std::errc::filename_too_long);
}

TEST_F(CppAuditNativeFileTest, ExactFileGrantDoesNotAuthorizeDirectoryEnumeration) {
    CppAuditReadPolicy exact;
    exact.files.push_back(root.generic_string());
    auto result = readCppAuditNativeDirectory(root.string(), exact, 16, 4096, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::permission_denied);
}

TEST_F(CppAuditNativeFileTest, DirectorySymlinkOutsideGrantIsDenied) {
    fs::create_directory(root / "allowed");
    fs::create_directory(root / "outside");
    write("outside/secret", "data");
    fs::create_directory_symlink(root / "outside", root / "allowed/link");
    CppAuditReadPolicy narrow;
    narrow.roots.push_back((root / "allowed").generic_string());
    auto result = readCppAuditNativeDirectory((root / "allowed/link").string(), narrow,
                                             16, 4096, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::permission_denied);
}

TEST_F(CppAuditNativeFileTest, RejectsRegularFileAsDirectory) {
    const auto file = write("file", "a");
    auto result = readCppAuditNativeDirectory(file.string(), policy, 16, 4096, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::not_a_directory);
}

#elif defined(_WIN32)

TEST_F(CppAuditNativeFileTest, WindowsDirectoryEnumerationReportsUnsupported) {
    auto result = readCppAuditNativeDirectory(root.string(), policy, 16, 4096, kPathLimit);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.getError(), std::errc::not_supported);
}

#endif
} // namespace
