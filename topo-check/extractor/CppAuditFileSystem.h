#ifndef TOPO_CPP_AUDIT_FILE_SYSTEM_H
#define TOPO_CPP_AUDIT_FILE_SYSTEM_H

#include "CppAuditNativeFile.h"
#include <map>
#include <optional>

namespace topo::check {

struct CppAuditFileSystemLimits {
    uint64_t maxFileBytes = 64 * 1024 * 1024;
    uint64_t maxTotalFileBytes = 256 * 1024 * 1024;
    size_t maxQueries = 65536;
    size_t maxPathBytes = 16384;
    size_t maxStringBytes = 8 * 1024 * 1024;
    size_t maxDirectoryEntries = 4096;
    size_t maxTotalDirectoryEntries = 32768;
};

enum class CppAuditFileQuery { Status, Open, RealPath, Directory, Locality };
struct CppAuditFileObservation {
    CppAuditFileQuery kind;
    std::string path;
    std::error_code error;
    std::optional<llvm::vfs::Status> status;
    std::string physicalPath;
    std::shared_ptr<const std::string> bytes;
    std::shared_ptr<const std::vector<llvm::vfs::directory_entry>> entries;
    std::error_code iterationError;
    bool local = false;
};

// One translation unit, one thread. Records each operation's first result and
// returns pinned bytes thereafter. Metadata queries never open file contents.
// Metadata visibility follows the OS sandbox; content and directory reads use
// only the separately supplied trusted policy. No cache input creates grants.
class CppAuditFileSystem final : public llvm::vfs::FileSystem {
public:
    static constexpr unsigned ObservationVersion = 1;
    CppAuditFileSystem(std::string workingDirectory, CppAuditReadPolicy policy,
                      CppAuditFileSystemLimits limits = {});
    ~CppAuditFileSystem() override;
    void seal();
    bool sealed() const { return sealed_; }
    bool complete() const { return failure_.empty(); }
    const std::string& failure() const { return failure_; }
    const std::vector<CppAuditFileObservation>& observations() const { return observations_; }
    uint64_t capturedBytes() const { return capturedBytes_; }

    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine&) override;
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine&) override;
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForReadBinary(const llvm::Twine&) override;
    llvm::vfs::directory_iterator dir_begin(const llvm::Twine&, std::error_code&) override;
    std::error_code getRealPath(const llvm::Twine&, llvm::SmallVectorImpl<char>&) override;
    std::error_code isLocal(const llvm::Twine&, bool&) override;
    std::error_code setCurrentWorkingDirectory(const llvm::Twine&) override;
    llvm::ErrorOr<std::string> getCurrentWorkingDirectory() const override;

private:
    using Key = std::pair<CppAuditFileQuery, std::string>;
    llvm::ErrorOr<std::string> absolute(const llvm::Twine&);
    llvm::ErrorOr<size_t> query(CppAuditFileQuery, const llvm::Twine&);
    bool chargeStrings(size_t);
    bool consistent(const std::string&, const llvm::vfs::Status&);
    std::error_code fail(const char*, std::errc);
    void record(size_t);
    std::string cwd_, failure_;
    CppAuditReadPolicy policy_;
    CppAuditFileSystemLimits limits_;
    std::unique_ptr<llvm::vfs::FileSystem> metadata_;
    bool sealed_ = false;
    uint64_t capturedBytes_ = 0;
    size_t stringBytes_ = 0, directoryEntries_ = 0;
    std::map<Key, size_t> indices_;
    std::map<std::string, llvm::vfs::Status> statuses_;
    std::map<llvm::sys::fs::UniqueID, CppAuditNativeSnapshot> identities_;
    std::vector<CppAuditFileObservation> observations_;
};
} // namespace topo::check
#endif
