#include "../../topo-check/extractor/CppAuditFileSystem.h"
#include <gtest/gtest.h>
#include <llvm/Support/MemoryBuffer.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace topo::check;
namespace {
class CppAuditFileSystemTest : public ::testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        llvm::SmallString<256> temporary;
        ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("topo-audit-fs-test", temporary));
        root = fs::canonical(temporary.str().str());
        fs::create_directory(root / "allowed");
    }
    void TearDown() override { fs::remove_all(root); }
    fs::path write(const std::string& name, const std::string& data) {
        auto path = root / name;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << data;
        return path;
    }
    std::unique_ptr<CppAuditFileSystem> recorder(CppAuditFileSystemLimits limits = {}) {
        return std::make_unique<CppAuditFileSystem>(root.string(), CppAuditReadPolicy{{(root / "allowed").string()}, {}}, limits);
    }
    std::string bytes(CppAuditFileSystem& vfs, const fs::path& path) {
        auto value = vfs.getBufferForFile(path.string());
        EXPECT_TRUE(bool(value)) << (value ? "" : value.getError().message());
        return value ? (*value)->getBuffer().str() : "";
    }
};
TEST_F(CppAuditFileSystemTest, MetadataDoesNotReadUnauthorizedContents) {
    auto path = write("outside.h", "secret");
    auto vfs = recorder();
    EXPECT_TRUE(bool(vfs->status(path.string())));
    EXPECT_EQ(vfs->capturedBytes(), 0u);
    ASSERT_EQ(vfs->observations().size(), 1u);
    EXPECT_FALSE(vfs->observations()[0].bytes);
    EXPECT_FALSE(bool(vfs->openFileForRead(path.string())));
    EXPECT_EQ(vfs->capturedBytes(), 0u);
    EXPECT_FALSE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, PinnedBytesOutliveFilesAndRecorder) {
    auto path = write("allowed/a.h", "old bytes");
    auto vfs = recorder();
    auto file = vfs->openFileForRead(path.string());
    ASSERT_TRUE(bool(file));
    auto buffer = (*file)->getBuffer("test", -1, true, true);
    ASSERT_TRUE(bool(buffer));
    (*file)->close();
    write("allowed/a.h", "new bytes");
    EXPECT_EQ(bytes(*vfs, path), "old bytes");
    vfs.reset();
    EXPECT_EQ((*buffer)->getBuffer(), "old bytes");
}
TEST_F(CppAuditFileSystemTest, SymlinkDotDotQueriesKeepDistinctPhysicalTargets) {
    write("allowed/left/value.h", "left");
    write("allowed/right/value.h", "right");
    fs::create_directory(root / "allowed/right/nested");
    fs::create_directory_symlink("../right/nested", root / "allowed/left/link");
    auto vfs = recorder();
    EXPECT_EQ(bytes(*vfs, root / "allowed/left/link/../value.h"), "right");
    EXPECT_EQ(bytes(*vfs, root / "allowed/left/value.h"), "left");
    EXPECT_NE(vfs->observations()[0].status->getUniqueID(), vfs->observations()[1].status->getUniqueID());
    EXPECT_NE(vfs->observations()[0].path.find("link/../"), std::string::npos);
}
TEST_F(CppAuditFileSystemTest, HardAndSoftAliasesShareBytesAndIdentity) {
    auto path = write("allowed/a.h", "one object");
    fs::create_hard_link(path, root / "allowed/hard.h");
    fs::create_symlink("a.h", root / "allowed/soft.h");
    auto vfs = recorder();
    for (auto name : {"a.h", "hard.h", "soft.h"}) EXPECT_EQ(bytes(*vfs, root / "allowed" / name), "one object");
    const auto& observations = vfs->observations();
    ASSERT_EQ(observations.size(), 3u);
    EXPECT_EQ(observations[0].bytes.get(), observations[1].bytes.get());
    EXPECT_EQ(observations[0].bytes.get(), observations[2].bytes.get());
}
TEST_F(CppAuditFileSystemTest, NegativeStatusPinsAbsentContent) {
    auto vfs = recorder();
    auto path = root / "allowed/later.h";
    auto before = vfs->status(path.string());
    ASSERT_FALSE(bool(before));
    write("allowed/later.h", "arrived");
    EXPECT_FALSE(bool(vfs->status(path.string())));
    EXPECT_FALSE(bool(vfs->openFileForRead(path.string())));
    EXPECT_EQ(vfs->capturedBytes(), 0u);
    EXPECT_TRUE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, NegativeOpenPinsAbsentStatus) {
    auto vfs = recorder();
    auto path = root / "allowed/later.h";
    ASSERT_FALSE(bool(vfs->openFileForRead(path.string())));
    write("allowed/later.h", "arrived");
    EXPECT_FALSE(bool(vfs->status(path.string())));
    EXPECT_TRUE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, SealRejectsUnseenOperationWithoutReading) {
    auto path = write("allowed/a.h", "before");
    auto vfs = recorder();
    ASSERT_TRUE(bool(vfs->status(path.string())));
    vfs->seal();
    EXPECT_TRUE(bool(vfs->status(path.string())));
    EXPECT_FALSE(bool(vfs->openFileForRead(path.string())));
    EXPECT_EQ(vfs->capturedBytes(), 0u);
    EXPECT_EQ(vfs->failure(), "unobserved query after seal");
}
TEST_F(CppAuditFileSystemTest, SealReplaysObservedBytesAndDirectory) {
    auto path = write("allowed/a.h", "before");
    auto vfs = recorder();
    EXPECT_EQ(bytes(*vfs, path), "before");
    std::error_code error;
    auto first = vfs->dir_begin((root / "allowed").string(), error);
    ASSERT_FALSE(error);
    ASSERT_NE(first, llvm::vfs::directory_iterator());
    vfs->seal();
    fs::remove(path);
    EXPECT_EQ(bytes(*vfs, path), "before");
    auto second = vfs->dir_begin((root / "allowed").string(), error);
    EXPECT_FALSE(error);
    ASSERT_NE(second, llvm::vfs::directory_iterator());
    EXPECT_EQ(first->path(), second->path());
    EXPECT_TRUE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, RejectsChangedFileBetweenStatusAndRead) {
    auto path = write("allowed/a.h", "a");
    auto vfs = recorder();
    ASSERT_TRUE(bool(vfs->status(path.string())));
    write("allowed/a.h", "larger");
    EXPECT_FALSE(bool(vfs->openFileForRead(path.string())));
    EXPECT_FALSE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, RejectsAliasMutationEvenWithRestoredTimestamp) {
    auto path = write("allowed/a.h", "one");
    auto hard = root / "allowed/hard.h";
    fs::create_hard_link(path, hard);
    auto vfs = recorder();
    EXPECT_EQ(bytes(*vfs, path), "one");
    auto timestamp = fs::last_write_time(path);
    write("allowed/a.h", "two");
    fs::last_write_time(path, timestamp);
    EXPECT_FALSE(bool(vfs->openFileForRead(hard.string())));
    EXPECT_EQ(vfs->failure(), "file bytes changed between aliases");
}
TEST_F(CppAuditFileSystemTest, RejectsSymlinkEscape) {
    auto outside = write("outside.h", "secret");
    fs::create_symlink(outside, root / "allowed/link.h");
    auto vfs = recorder();
    EXPECT_FALSE(bool(vfs->openFileForRead((root / "allowed/link.h").string())));
    EXPECT_EQ(vfs->capturedBytes(), 0u);
}
TEST_F(CppAuditFileSystemTest, RejectsRetargetingBetweenRealPathAndRead) {
    auto path = write("allowed/a.h", "a");
    auto other = write("allowed/b.h", "b");
    auto link = root / "allowed/link.h";
    fs::create_symlink(path, link);
    auto vfs = recorder();
    llvm::SmallString<256> real;
    ASSERT_FALSE(vfs->getRealPath(link.string(), real));
    fs::remove(link);
    fs::create_symlink(other, link);
    EXPECT_FALSE(bool(vfs->openFileForRead(link.string())));
    EXPECT_FALSE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, FileAndCumulativeByteLimits) {
    auto a = write("allowed/a.h", "1234");
    auto b = write("allowed/b.h", "5678");
    CppAuditFileSystemLimits limits;
    limits.maxFileBytes = 3;
    auto small = recorder(limits);
    EXPECT_FALSE(bool(small->openFileForRead(a.string())));
    limits.maxFileBytes = 4;
    limits.maxTotalFileBytes = 7;
    auto total = recorder(limits);
    EXPECT_EQ(bytes(*total, a), "1234");
    EXPECT_FALSE(bool(total->openFileForRead(b.string())));
    EXPECT_EQ(total->capturedBytes(), 4u);
}
TEST_F(CppAuditFileSystemTest, PathQueryAndStringLimits) {
    CppAuditFileSystemLimits limits;
    limits.maxQueries = 1;
    auto queries = recorder(limits);
    EXPECT_FALSE(bool(queries->status("allowed/missing")));
    EXPECT_FALSE(bool(queries->status("allowed/missing2")));
    EXPECT_EQ(queries->observations().size(), 1u);
    EXPECT_FALSE(queries->complete());
    limits = {};
    limits.maxStringBytes = 2;
    auto strings = recorder(limits);
    EXPECT_FALSE(bool(strings->status("allowed/a")));
    EXPECT_TRUE(strings->observations().empty());
    limits = {};
    limits.maxPathBytes = root.string().size() + 4;
    auto paths = recorder(limits);
    EXPECT_FALSE(bool(paths->status("allowed/longer.h")));
    EXPECT_TRUE(paths->observations().empty());
    auto nul = recorder();
    EXPECT_FALSE(bool(nul->status(std::string("allowed/a\0b", 11))));
}
TEST_F(CppAuditFileSystemTest, DirectoryLimitsRejectPartialResults) {
    write("allowed/a.h", "a");
    write("allowed/b.h", "b");
    CppAuditFileSystemLimits limits;
    limits.maxDirectoryEntries = 1;
    auto vfs = recorder(limits);
    std::error_code error;
    EXPECT_EQ(vfs->dir_begin((root / "allowed").string(), error), llvm::vfs::directory_iterator());
    EXPECT_TRUE(bool(error));
    EXPECT_FALSE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, WorkingDirectoryRemainsExplicitAndImmutable) {
    auto vfs = recorder();
    ASSERT_TRUE(bool(vfs->getCurrentWorkingDirectory()));
    EXPECT_EQ(*vfs->getCurrentWorkingDirectory(), root.string());
    EXPECT_FALSE(vfs->setCurrentWorkingDirectory(root.string()));
    EXPECT_TRUE(bool(vfs->setCurrentWorkingDirectory((root / "allowed").string())));
    EXPECT_FALSE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, MissingRealPathPinsLaterStatusAndRead) {
    auto path = root / "allowed/later.h";
    auto vfs = recorder();
    llvm::SmallString<256> real;
    EXPECT_TRUE(bool(vfs->getRealPath(path.string(), real)));
    write("allowed/later.h", "arrived");
    EXPECT_FALSE(bool(vfs->status(path.string())));
    EXPECT_FALSE(bool(vfs->openFileForRead(path.string())));
    EXPECT_EQ(vfs->capturedBytes(), 0u);
    EXPECT_TRUE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, MissingDirectoryPinsLaterStatus) {
    auto path = root / "allowed/later";
    auto vfs = recorder();
    std::error_code error;
    EXPECT_EQ(vfs->dir_begin(path.string(), error), llvm::vfs::directory_iterator());
    EXPECT_EQ(error, std::errc::no_such_file_or_directory);
    fs::create_directory(path);
    EXPECT_FALSE(bool(vfs->status(path.string())));
    EXPECT_TRUE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, MetadataRealPathStaysPinnedAfterRetargeting) {
    auto path = write("allowed/a.h", "a");
    auto other = write("allowed/b.h", "b");
    auto link = root / "allowed/link.h";
    fs::create_symlink(path, link);
    auto vfs = recorder();
    ASSERT_TRUE(bool(vfs->status(link.string())));
    fs::remove(link);
    fs::create_symlink(other, link);
    llvm::SmallString<256> real;
    ASSERT_FALSE(vfs->getRealPath(link.string(), real));
    EXPECT_EQ(real.str(), path.string());
    EXPECT_FALSE(bool(vfs->openFileForRead(link.string())));
    EXPECT_FALSE(vfs->complete());
}

TEST_F(CppAuditFileSystemTest, DirectoryTypeErrorDoesNotHideRegularFile) {
    auto path = write("allowed/file.h", "ordinary file");
    auto vfs = recorder();
    std::error_code error;
    EXPECT_EQ(vfs->dir_begin(path.string(), error), llvm::vfs::directory_iterator());
    EXPECT_EQ(error, std::errc::not_a_directory);
    EXPECT_TRUE(bool(vfs->status(path.string())));
    EXPECT_EQ(bytes(*vfs, path), "ordinary file");
    EXPECT_TRUE(vfs->complete());
}

TEST_F(CppAuditFileSystemTest, ExistingStatusThenMissingOpenIsIncomplete) {
    auto path = write("allowed/file.h", "ordinary file");
    auto vfs = recorder();
    ASSERT_TRUE(bool(vfs->status(path.string())));
    fs::remove(path);
    EXPECT_FALSE(bool(vfs->openFileForRead(path.string())));
    EXPECT_FALSE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, ExistingRealPathThenMissingStatusIsIncomplete) {
    auto path = write("allowed/file.h", "ordinary file");
    auto vfs = recorder();
    llvm::SmallString<256> real;
    ASSERT_FALSE(vfs->getRealPath(path.string(), real));
    fs::remove(path);
    EXPECT_FALSE(bool(vfs->status(path.string())));
    EXPECT_FALSE(vfs->complete());
}
TEST_F(CppAuditFileSystemTest, ExistingDirectoryThenReplacedByFileIsIncomplete) {
    auto path = root / "allowed/dir";
    fs::create_directory(path);
    auto vfs = recorder();
    ASSERT_TRUE(bool(vfs->status(path.string())));
    fs::remove(path);
    write("allowed/dir", "ordinary file");
    std::error_code error;
    EXPECT_EQ(vfs->dir_begin(path.string(), error), llvm::vfs::directory_iterator());
    EXPECT_EQ(error, std::errc::state_not_recoverable);
    EXPECT_FALSE(vfs->complete());
}

}
