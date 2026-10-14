#include "CppAuditFileSystem.h"
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <algorithm>
#include <limits>

namespace topo::check {
namespace {
bool sameMetadata(const llvm::vfs::Status& a, const llvm::vfs::Status& b) {
    return a.getUniqueID() == b.getUniqueID() && a.getType() == b.getType() &&
        a.getSize() == b.getSize() && a.getLastModificationTime() == b.getLastModificationTime() &&
        a.getPermissions() == b.getPermissions() && a.getUser() == b.getUser() &&
        a.getGroup() == b.getGroup();
}
class SharedBuffer final : public llvm::MemoryBuffer {
    std::shared_ptr<const std::string> bytes_;
    std::string name_;
public:
    SharedBuffer(std::shared_ptr<const std::string> bytes, std::string name)
        : bytes_(std::move(bytes)), name_(std::move(name)) {
        init(bytes_->data(), bytes_->data() + bytes_->size(), true);
    }
    llvm::StringRef getBufferIdentifier() const override { return name_; }
    BufferKind getBufferKind() const override { return MemoryBuffer_Malloc; }
};
class PinnedFile final : public llvm::vfs::File {
    llvm::vfs::Status status_;
    std::shared_ptr<const std::string> bytes_;
    size_t maxPathBytes_;
    bool closed_ = false;
public:
    PinnedFile(const CppAuditFileObservation& observation, size_t maxPathBytes)
        : status_(*observation.status), bytes_(observation.bytes), maxPathBytes_(maxPathBytes) {}
    llvm::ErrorOr<llvm::vfs::Status> status() override {
        if (closed_) return std::make_error_code(std::errc::bad_file_descriptor);
        return status_;
    }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(
        const llvm::Twine& name, int64_t, bool, bool) override {
        if (closed_) return std::make_error_code(std::errc::bad_file_descriptor);
        auto text = name.str();
        if (text.size() > maxPathBytes_ || text.find('\0') != std::string::npos)
            return std::make_error_code(std::errc::filename_too_long);
        return std::unique_ptr<llvm::MemoryBuffer>(new SharedBuffer(bytes_, std::move(text)));
    }
    std::error_code close() override { closed_ = true; return {}; }
};
class PinnedDirectory final : public llvm::vfs::detail::DirIterImpl {
    std::shared_ptr<const std::vector<llvm::vfs::directory_entry>> entries_;
    std::error_code tail_;
    size_t index_ = 0;
public:
    explicit PinnedDirectory(const CppAuditFileObservation& observation)
        : entries_(observation.entries), tail_(observation.iterationError) {
        if (entries_ && !entries_->empty()) CurrentEntry = entries_->front();
    }
    std::error_code increment() override {
        if (++index_ < entries_->size()) { CurrentEntry = (*entries_)[index_]; return {}; }
        CurrentEntry = {};
        return tail_;
    }
};
}

CppAuditFileSystem::CppAuditFileSystem(std::string workingDirectory,
        CppAuditReadPolicy policy, CppAuditFileSystemLimits limits)
    : cwd_(std::move(workingDirectory)), policy_(std::move(policy)), limits_(limits),
      metadata_(llvm::vfs::createPhysicalFileSystem()) {
    if (cwd_.size() > limits_.maxPathBytes || cwd_.find('\0') != std::string::npos ||
        !llvm::sys::path::is_absolute(cwd_))
        fail("invalid working directory", std::errc::invalid_argument);
    else if (auto error = metadata_->setCurrentWorkingDirectory(cwd_))
        fail("unavailable working directory", std::errc::invalid_argument);
}
CppAuditFileSystem::~CppAuditFileSystem() = default;
void CppAuditFileSystem::seal() { sealed_ = true; }
std::error_code CppAuditFileSystem::fail(const char* reason, std::errc error) {
    if (failure_.empty()) failure_ = reason;
    return std::make_error_code(error);
}
bool CppAuditFileSystem::chargeStrings(size_t bytes) {
    if (bytes > limits_.maxStringBytes - std::min(stringBytes_, limits_.maxStringBytes)) {
        fail("observation string limit exceeded", std::errc::file_too_large);
        return false;
    }
    stringBytes_ += bytes;
    return true;
}
llvm::ErrorOr<std::string> CppAuditFileSystem::absolute(const llvm::Twine& path) {
    auto text = path.str();
    if (text.find('\0') != std::string::npos)
        return fail("path contains null", std::errc::invalid_argument);
    if (text.size() > limits_.maxPathBytes)
        return fail("path limit exceeded", std::errc::filename_too_long);
    llvm::SmallString<256> result(text);
    if (!llvm::sys::path::is_absolute(result)) {
        result = cwd_;
        // Deliberately retain dots: symlink/.. has physical lookup semantics.
        llvm::sys::path::append(result, text);
    }
    if (result.size() > limits_.maxPathBytes)
        return fail("absolute path limit exceeded", std::errc::filename_too_long);
    return result.str().str();
}
llvm::ErrorOr<size_t> CppAuditFileSystem::query(CppAuditFileQuery kind, const llvm::Twine& path) {
    if (!complete()) return std::make_error_code(std::errc::state_not_recoverable);
    auto name = absolute(path);
    if (!name) return name.getError();
    Key key{kind, *name};
    auto found = indices_.find(key);
    if (found != indices_.end()) return found->second;
    if (sealed_) return fail("unobserved query after seal", std::errc::operation_not_permitted);
    if (observations_.size() >= limits_.maxQueries)
        return fail("observation count limit exceeded", std::errc::file_too_large);
    // Names occur in the index, observation, status and identity records. This
    // conservative charge bounds retained strings without relying on SSO.
    if (name->size() > std::numeric_limits<size_t>::max() / 8 || !chargeStrings(8 * name->size()))
        return std::make_error_code(std::errc::file_too_large);
    const size_t index = observations_.size();
    indices_.emplace(std::move(key), index);
    CppAuditFileObservation observation{};
    observation.kind = kind;
    observation.path = std::move(*name);
    observations_.push_back(std::move(observation));
    record(index);
    return index;
}
bool CppAuditFileSystem::consistent(const std::string& path, const llvm::vfs::Status& status) {
    auto old = statuses_.find(path);
    if (old != statuses_.end() && !sameMetadata(old->second, status)) {
        fail("file metadata changed between observations", std::errc::state_not_recoverable);
        return false;
    }
    auto alias = identities_.find(status.getUniqueID());
    if (alias != identities_.end() && !sameMetadata(alias->second.status, status)) {
        fail("file identity changed between aliases", std::errc::state_not_recoverable);
        return false;
    }
    statuses_.emplace(path, status);
    return true;
}
void CppAuditFileSystem::record(size_t index) {
    auto& result = observations_[index];
    const auto& path = result.path;
    const std::string* priorPhysical = nullptr;
    for (auto kind : {CppAuditFileQuery::Status, CppAuditFileQuery::Open,
                      CppAuditFileQuery::RealPath, CppAuditFileQuery::Directory}) {
        auto prior = indices_.find({kind, path});
        if (prior == indices_.end() || prior->second == index) continue;
        const auto& previous = observations_[prior->second];
        if (result.kind != CppAuditFileQuery::Locality &&
            (previous.error == std::errc::no_such_file_or_directory ||
             (kind != CppAuditFileQuery::Directory && previous.error == std::errc::not_a_directory))) {
            result.error = previous.error;
            return;
        }
        if (!previous.error && !previous.physicalPath.empty()) priorPhysical = &previous.physicalPath;
    }
    switch (result.kind) {
    case CppAuditFileQuery::Status: {
        auto prior = statuses_.find(path);
        if (prior != statuses_.end()) { result.status = prior->second; break; }
        auto opened = indices_.find({CppAuditFileQuery::Open, path});
        if (opened != indices_.end()) {
            auto error = observations_[opened->second].error;
            if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
                result.error = error; break;
            }
        }
        auto current = metadata_->status(path);
        if (!current) result.error = current.getError();
        else if (!consistent(path, *current)) result.error = std::make_error_code(std::errc::state_not_recoverable);
        else {
            llvm::SmallString<256> physical;
            result.error = metadata_->getRealPath(path, physical);
            auto after = metadata_->status(path);
            if (!result.error && (!after || !sameMetadata(*current, *after)))
                result.error = fail("metadata target changed while resolving", std::errc::state_not_recoverable);
            if (!result.error && (physical.size() > limits_.maxPathBytes || !chargeStrings(physical.size())))
                result.error = fail("resolved path limit exceeded", std::errc::filename_too_long);
            if (!result.error) {
                result.status = *current;
                result.physicalPath = physical.str().str();
            }
        }
        break;
    }
    case CppAuditFileQuery::Open: {
        auto negative = indices_.find({CppAuditFileQuery::Status, path});
        if (negative != indices_.end() && observations_[negative->second].error) {
            result.error = observations_[negative->second].error;
            break;
        }
        auto remaining = limits_.maxTotalFileBytes - std::min(capturedBytes_, limits_.maxTotalFileBytes);
        auto current = readCppAuditNativeFile(path, policy_, std::min(remaining, limits_.maxFileBytes), limits_.maxPathBytes);
        if (!current) {
            result.error = current.getError();
            if (result.error == std::errc::file_too_large)
                fail("file byte limit exceeded", std::errc::file_too_large);
            else if (result.error == std::errc::state_not_recoverable)
                fail("file changed while reading", std::errc::state_not_recoverable);
            else if (result.error == std::errc::permission_denied)
                fail("content access denied", std::errc::permission_denied);
            break;
        }
        auto resolved = indices_.find({CppAuditFileQuery::RealPath, path});
        if (resolved != indices_.end()) {
            const auto& earlier = observations_[resolved->second];
            if (earlier.error || earlier.physicalPath != current->physicalPath) {
                result.error = fail("physical target changed before read", std::errc::state_not_recoverable); break;
            }
        }
        if (!chargeStrings(2 * current->physicalPath.size()) || !consistent(path, current->status)) {
            result.error = std::make_error_code(std::errc::state_not_recoverable); break;
        }
        capturedBytes_ += current->bytes->size();
        auto alias = identities_.find(current->status.getUniqueID());
        if (alias != identities_.end()) {
            if (*alias->second.bytes != *current->bytes) {
                result.error = fail("file bytes changed between aliases", std::errc::state_not_recoverable); break;
            }
            current->bytes = alias->second.bytes;
        } else identities_.emplace(current->status.getUniqueID(), *current);
        result.status = current->status;
        result.physicalPath = std::move(current->physicalPath);
        result.bytes = std::move(current->bytes);
        break;
    }
    case CppAuditFileQuery::RealPath: {
        if (priorPhysical) {
            if (chargeStrings(priorPhysical->size())) result.physicalPath = *priorPhysical;
            else result.error = std::make_error_code(std::errc::file_too_large);
            break;
        }
        llvm::SmallString<256> real;
        result.error = metadata_->getRealPath(path, real);
        if (!result.error) {
            if (real.size() > limits_.maxPathBytes || !chargeStrings(real.size()))
                result.error = fail("resolved path limit exceeded", std::errc::filename_too_long);
            else result.physicalPath = real.str().str();
        }
        break;
    }
    case CppAuditFileQuery::Directory: {
        const size_t remaining = limits_.maxTotalDirectoryEntries - std::min(directoryEntries_, limits_.maxTotalDirectoryEntries);
        auto current = readCppAuditNativeDirectory(path, policy_, std::min(remaining, limits_.maxDirectoryEntries),
            limits_.maxStringBytes - std::min(stringBytes_, limits_.maxStringBytes), limits_.maxPathBytes);
        if (!current) {
            result.error = current.getError();
            if (result.error != std::errc::no_such_file_or_directory && result.error != std::errc::not_a_directory)
                fail("directory observation failed", std::errc::state_not_recoverable);
            break;
        }
        if (!consistent(path, current->status) || !chargeStrings(current->physicalPath.size())) {
            result.error = std::make_error_code(std::errc::state_not_recoverable); break;
        }
        for (const auto& entry : current->entries) if (!chargeStrings(entry.path().size())) {
            result.error = std::make_error_code(std::errc::file_too_large); return;
        }
        directoryEntries_ += current->entries.size();
        result.status = current->status;
        result.physicalPath = std::move(current->physicalPath);
        result.entries = std::make_shared<const std::vector<llvm::vfs::directory_entry>>(std::move(current->entries));
        result.iterationError = current->tailError;
        if (result.iterationError) fail("directory iteration incomplete", std::errc::state_not_recoverable);
        break;
    }
    case CppAuditFileQuery::Locality:
        result.error = metadata_->isLocal(path, result.local);
        break;
    }
    if (!result.error && priorPhysical && !result.physicalPath.empty() && *priorPhysical != result.physicalPath)
        result.error = fail("physical target changed between observations", std::errc::state_not_recoverable);
    const auto known = statuses_.find(path);
    const bool hadPositive = priorPhysical || known != statuses_.end();
    const bool lostParent = result.error == std::errc::not_a_directory &&
        (result.kind != CppAuditFileQuery::Directory ||
         (known != statuses_.end() && known->second.isDirectory()));
    if (hadPositive && (result.error == std::errc::no_such_file_or_directory || lostParent))
        result.error = fail("existing input disappeared between observations", std::errc::state_not_recoverable);
}
llvm::ErrorOr<llvm::vfs::Status> CppAuditFileSystem::status(const llvm::Twine& path) {
    auto index = query(CppAuditFileQuery::Status, path);
    if (!index) return index.getError();
    const auto& observation = observations_[*index];
    if (observation.error) return observation.error;
    return *observation.status;
}
llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> CppAuditFileSystem::openFileForRead(const llvm::Twine& path) {
    auto index = query(CppAuditFileQuery::Open, path);
    if (!index) return index.getError();
    const auto& observation = observations_[*index];
    if (observation.error) return observation.error;
    return std::unique_ptr<llvm::vfs::File>(new PinnedFile(observation, limits_.maxPathBytes));
}
llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> CppAuditFileSystem::openFileForReadBinary(const llvm::Twine& path) {
    return openFileForRead(path);
}
llvm::vfs::directory_iterator CppAuditFileSystem::dir_begin(const llvm::Twine& path, std::error_code& error) {
    auto index = query(CppAuditFileQuery::Directory, path);
    if (!index) { error = index.getError(); return {}; }
    const auto& observation = observations_[*index];
    error = observation.error;
    if (error) return {};
    if (!observation.entries || observation.entries->empty()) { error = observation.iterationError; return {}; }
    return llvm::vfs::directory_iterator(std::make_shared<PinnedDirectory>(observation));
}
std::error_code CppAuditFileSystem::getRealPath(const llvm::Twine& path, llvm::SmallVectorImpl<char>& output) {
    auto index = query(CppAuditFileQuery::RealPath, path);
    if (!index) return index.getError();
    const auto& observation = observations_[*index];
    if (!observation.error) output.assign(observation.physicalPath.begin(), observation.physicalPath.end());
    return observation.error;
}
std::error_code CppAuditFileSystem::isLocal(const llvm::Twine& path, bool& local) {
    auto index = query(CppAuditFileQuery::Locality, path);
    if (!index) return index.getError();
    const auto& observation = observations_[*index];
    if (!observation.error) local = observation.local;
    return observation.error;
}
std::error_code CppAuditFileSystem::setCurrentWorkingDirectory(const llvm::Twine& path) {
    auto name = absolute(path);
    if (!name) return name.getError();
    if (*name != cwd_) return fail("working directory change unsupported", std::errc::operation_not_supported);
    return {};
}
llvm::ErrorOr<std::string> CppAuditFileSystem::getCurrentWorkingDirectory() const {
    if (!complete()) return std::make_error_code(std::errc::state_not_recoverable);
    return cwd_;
}
} // namespace topo::check
