#include "CppAuditNativeFile.h"

#include <llvm/Support/Path.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace topo::check {
namespace {

std::error_code error(std::errc value) {
    return std::make_error_code(value);
}

std::error_code validateSpelling(const std::string& spelling, size_t limit) {
    if (spelling.size() > limit)
        return error(std::errc::filename_too_long);
    if (spelling.find('\0') != std::string::npos ||
        !llvm::sys::path::is_absolute(spelling))
        return error(std::errc::invalid_argument);
    return {};
}

// Physical policy paths have already been resolved by the trusted caller.
// Do not remove dot components from the spelling being opened: a symlink
// followed by ".." must be traversed by the operating system.
std::string policySpelling(std::string path) {
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.compare(0, 8, "//?/UNC/") == 0)
        path = "//" + path.substr(8);
    else if (path.compare(0, 4, "//?/") == 0)
        path.erase(0, 4);
#endif
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

bool authorized(const std::string& physical, const CppAuditReadPolicy& policy,
                bool directory) {
    const std::string target = policySpelling(physical);
    for (const auto& value : policy.roots) {
        const std::string root = policySpelling(value);
        if (root.empty())
            continue;
        if (target == root ||
            (target.size() > root.size() &&
             target.compare(0, root.size(), root) == 0 &&
             (root.back() == '/' || target[root.size()] == '/')))
            return true;
    }
    if (!directory)
        for (const auto& value : policy.files)
            if (target == policySpelling(value))
                return true;
    return false;
}

#if defined(__APPLE__) || defined(__linux__)

struct NativeHandle {
    int fd = -1;
    explicit NativeHandle(int value) : fd(value) {}
    ~NativeHandle() {
        if (fd != -1)
            ::close(fd);
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
};

std::error_code nativeError() {
    return {errno, std::generic_category()};
}

llvm::ErrorOr<std::string> physicalPath(int fd, size_t limit) {
#ifdef __APPLE__
    // F_GETPATH requires MAXPATHLEN bytes regardless of the caller's budget.
    // Its fixed stack buffer is bounded independently of untrusted input.
    std::array<char, MAXPATHLEN> buffer{};
    if (::fcntl(fd, F_GETPATH, buffer.data()) == -1)
        return nativeError();
    const size_t length = ::strnlen(buffer.data(), buffer.size());
    if (length == buffer.size() || length > limit)
        return error(std::errc::filename_too_long);
    std::string path(buffer.data(), length);
#else
    // Linux has no F_GETPATH; procfs resolves the already-open descriptor.
    // A missing procfs mount is an explicit error, never a path-based retry.
    std::array<char, 4096> buffer{};
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    const ssize_t length = ::readlink(link.c_str(), buffer.data(), buffer.size());
    if (length < 0)
        return nativeError();
    if (static_cast<size_t>(length) == buffer.size() ||
        static_cast<size_t>(length) > limit)
        return error(std::errc::filename_too_long);
    std::string path(buffer.data(), static_cast<size_t>(length));
    // The procfs suffix is ambiguous with a literal filename. Reject both
    // instead of authorizing a deleted object through an invented path.
    constexpr const char* deleted = " (deleted)";
    if (path.size() >= std::strlen(deleted) &&
        path.compare(path.size() - std::strlen(deleted), std::strlen(deleted),
                     deleted) == 0)
        return error(std::errc::state_not_recoverable);
#endif
    if (path.empty() || path.front() != '/')
        return error(std::errc::not_supported);
    return path;
}

timespec modificationTime(const struct stat& value) {
#ifdef __APPLE__
    return value.st_mtimespec;
#else
    return value.st_mtim;
#endif
}

timespec changeTime(const struct stat& value) {
#ifdef __APPLE__
    return value.st_ctimespec;
#else
    return value.st_ctim;
#endif
}

bool equalTime(timespec left, timespec right) {
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

bool sameObjectState(const struct stat& left, const struct stat& right) {
    // atime can change because of our own read and is deliberately excluded.
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_mode == right.st_mode && left.st_uid == right.st_uid &&
           left.st_gid == right.st_gid && left.st_nlink == right.st_nlink &&
           left.st_size == right.st_size &&
           equalTime(modificationTime(left), modificationTime(right)) &&
           equalTime(changeTime(left), changeTime(right));
}

llvm::vfs::Status makeStatus(const std::string& spelling, const struct stat& s) {
    const auto mtime = modificationTime(s);
    return llvm::vfs::Status(
        spelling, llvm::sys::fs::UniqueID(s.st_dev, s.st_ino),
        llvm::sys::TimePoint<>(std::chrono::seconds(mtime.tv_sec) +
                               std::chrono::nanoseconds(mtime.tv_nsec)),
        s.st_uid, s.st_gid, static_cast<uint64_t>(s.st_size),
        S_ISDIR(s.st_mode) ? llvm::sys::fs::file_type::directory_file
                          : llvm::sys::fs::file_type::regular_file,
        static_cast<llvm::sys::fs::perms>(s.st_mode & 07777));
}

llvm::sys::fs::file_type entryType(unsigned char type) {
    using Type = llvm::sys::fs::file_type;
    switch (type) {
    case DT_REG: return Type::regular_file;
    case DT_DIR: return Type::directory_file;
    case DT_LNK: return Type::symlink_file;
    case DT_FIFO: return Type::fifo_file;
    case DT_CHR: return Type::character_file;
    case DT_BLK: return Type::block_file;
    case DT_SOCK: return Type::socket_file;
    default: return Type::type_unknown;
    }
}

#elif defined(_WIN32)

struct NativeHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit NativeHandle(HANDLE value) : handle(value) {}
    ~NativeHandle() {
        if (handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
};

std::error_code nativeError() {
    return {static_cast<int>(::GetLastError()), std::system_category()};
}

llvm::ErrorOr<std::wstring> widePath(const std::string& path) {
    if (path.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return error(std::errc::filename_too_long);
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            path.data(), static_cast<int>(path.size()),
                                            nullptr, 0);
    if (length == 0)
        return nativeError();
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                             static_cast<int>(path.size()), result.data(), length) != length)
        return nativeError();
    return result;
}

llvm::ErrorOr<std::string> physicalPath(HANDLE handle, size_t limit) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD size = ::GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (size == 0)
        return nativeError();
    // The wide and UTF-8 allocations are both bounded before allocating.
    if (size > limit || size > static_cast<DWORD>(std::numeric_limits<int>::max()))
        return error(std::errc::filename_too_long);
    std::wstring buffer(size, L'\0');
    const DWORD used = ::GetFinalPathNameByHandleW(handle, buffer.data(), size, flags);
    if (used == 0)
        return nativeError();
    if (used >= size)
        return error(std::errc::state_not_recoverable);
    const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          buffer.data(), static_cast<int>(used),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes == 0)
        return nativeError();
    if (static_cast<size_t>(bytes) > limit)
        return error(std::errc::filename_too_long);
    std::string result(static_cast<size_t>(bytes), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer.data(),
                             static_cast<int>(used), result.data(), bytes,
                             nullptr, nullptr) != bytes)
        return nativeError();
    return policySpelling(std::move(result));
}

struct NativeState {
    BY_HANDLE_FILE_INFORMATION identity{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    llvm::sys::fs::file_status status;
};

std::error_code getState(HANDLE handle, NativeState& state) {
    if (::GetFileType(handle) != FILE_TYPE_DISK)
        return error(std::errc::not_supported);
    if (!::GetFileInformationByHandle(handle, &state.identity) ||
        !::GetFileInformationByHandleEx(handle, FileBasicInfo, &state.basic,
                                       sizeof(state.basic)) ||
        !::GetFileInformationByHandleEx(handle, FileStandardInfo, &state.standard,
                                       sizeof(state.standard)))
        return nativeError();
    if (state.standard.Directory)
        return error(std::errc::is_a_directory);
    return llvm::sys::fs::status(handle, state.status);
}

bool sameObjectState(const NativeState& left, const NativeState& right) {
    return left.identity.dwVolumeSerialNumber == right.identity.dwVolumeSerialNumber &&
           left.identity.nFileIndexHigh == right.identity.nFileIndexHigh &&
           left.identity.nFileIndexLow == right.identity.nFileIndexLow &&
           left.basic.CreationTime.QuadPart == right.basic.CreationTime.QuadPart &&
           left.basic.LastWriteTime.QuadPart == right.basic.LastWriteTime.QuadPart &&
           left.basic.ChangeTime.QuadPart == right.basic.ChangeTime.QuadPart &&
           left.basic.FileAttributes == right.basic.FileAttributes &&
           left.standard.EndOfFile.QuadPart == right.standard.EndOfFile.QuadPart &&
           left.standard.NumberOfLinks == right.standard.NumberOfLinks &&
           left.standard.DeletePending == right.standard.DeletePending &&
           left.status.getUniqueID() == right.status.getUniqueID() &&
           left.status.getSize() == right.status.getSize() &&
           left.status.getLastModificationTime() == right.status.getLastModificationTime();
}

#endif

} // namespace

llvm::ErrorOr<CppAuditNativeSnapshot> readCppAuditNativeFile(
    const std::string& absoluteSpelling, const CppAuditReadPolicy& policy,
    uint64_t maxBytes, size_t maxPathBytes) {
    if (auto ec = validateSpelling(absoluteSpelling, maxPathBytes))
        return ec;
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
#ifdef _WIN32
    auto wide = widePath(absoluteSpelling);
    if (!wide)
        return wide.getError();
    NativeHandle handle(::CreateFileW(wide->c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr));
    if (handle.handle == INVALID_HANDLE_VALUE)
        return nativeError();
    NativeState before;
    if (auto ec = getState(handle.handle, before))
        return ec;
    auto physical = physicalPath(handle.handle, maxPathBytes);
    const uint64_t size = before.status.getSize();
#else
    NativeHandle handle(::open(absoluteSpelling.c_str(),
                              O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY));
    if (handle.fd == -1)
        return nativeError();
    struct stat before{};
    if (::fstat(handle.fd, &before) == -1)
        return nativeError();
    if (!S_ISREG(before.st_mode))
        return error(S_ISDIR(before.st_mode) ? std::errc::is_a_directory
                                             : std::errc::not_supported);
    if (before.st_size < 0)
        return error(std::errc::not_supported);
    auto physical = physicalPath(handle.fd, maxPathBytes);
    const uint64_t size = static_cast<uint64_t>(before.st_size);
#endif
    if (!physical)
        return physical.getError();
    if (!authorized(*physical, policy, false))
        return error(std::errc::permission_denied);
    std::string bytes;
    if (size > maxBytes || size > bytes.max_size())
        return error(std::errc::file_too_large);
    bytes.resize(static_cast<size_t>(size));
    char overflowByte = 0;
    uint64_t remaining = size;
    // Size is authorized and bounded before the one content allocation. The
    // final single-byte EOF probe detects growth and pseudo-regular files with
    // an inaccurate size.
    while (true) {
        const size_t requested = remaining == 0 ? 1 :
            static_cast<size_t>(std::min<uint64_t>(remaining, 64 * 1024));
        char* destination = remaining == 0 ? &overflowByte :
            bytes.data() + static_cast<size_t>(size - remaining);
#ifdef _WIN32
        DWORD count = 0;
        // ReadFile keeps CR/LF and Ctrl-Z bytes intact; no CRT text mode.
        if (!::ReadFile(handle.handle, destination, static_cast<DWORD>(requested),
                        &count, nullptr))
            return nativeError();
#else
        ssize_t count;
        do {
            count = ::read(handle.fd, destination, requested);
        } while (count == -1 && errno == EINTR);
        if (count < 0)
            return nativeError();
#endif
        if (count == 0) {
            if (remaining != 0)
                return error(std::errc::state_not_recoverable);
            break;
        }
        if (remaining == 0)
            return error(std::errc::state_not_recoverable);
        remaining -= static_cast<uint64_t>(count);
    }
#ifdef _WIN32
    NativeState after;
    if (auto ec = getState(handle.handle, after))
        return ec;
    auto finalPath = physicalPath(handle.handle, maxPathBytes);
    const auto status = llvm::vfs::Status::copyWithNewName(before.status, absoluteSpelling);
#else
    struct stat after{};
    if (::fstat(handle.fd, &after) == -1)
        return nativeError();
    auto finalPath = physicalPath(handle.fd, maxPathBytes);
    const auto status = makeStatus(absoluteSpelling, before);
#endif
    if (!sameObjectState(before, after) || !finalPath || *finalPath != *physical)
        return error(std::errc::state_not_recoverable);
    return CppAuditNativeSnapshot{status, std::move(*physical),
                                 std::make_shared<const std::string>(std::move(bytes))};
#else
    (void)policy;
    (void)maxBytes;
    return error(std::errc::not_supported);
#endif
}

llvm::ErrorOr<CppAuditNativeDirectory> readCppAuditNativeDirectory(
    const std::string& absoluteSpelling, const CppAuditReadPolicy& policy,
    size_t maxEntries, size_t maxStringBytes, size_t maxPathBytes) {
    if (auto ec = validateSpelling(absoluteSpelling, maxPathBytes))
        return ec;
#if defined(__APPLE__) || defined(__linux__)
    NativeHandle handle(::open(absoluteSpelling.c_str(),
                              O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY | O_DIRECTORY));
    if (handle.fd == -1)
        return nativeError();
    struct stat before{};
    if (::fstat(handle.fd, &before) == -1)
        return nativeError();
    if (!S_ISDIR(before.st_mode))
        return error(std::errc::not_a_directory);
    auto physical = physicalPath(handle.fd, maxPathBytes);
    if (!physical)
        return physical.getError();
    if (!authorized(*physical, policy, true))
        return error(std::errc::permission_denied);
    const int fd = handle.fd;
    DIR* raw = ::fdopendir(fd);
    if (!raw)
        return nativeError();
    handle.fd = -1; // fdopendir owns this same authorized descriptor.
    const std::unique_ptr<DIR, decltype(&::closedir)> directory(raw, &::closedir);
    CppAuditNativeDirectory result{makeStatus(absoluteSpelling, before),
                                  std::move(*physical), {}, {}};
    size_t stringBytes = 0;
    while (true) {
        errno = 0;
        const dirent* entry = ::readdir(directory.get());
        if (!entry) {
            if (errno != 0)
                result.tailError = nativeError();
            break;
        }
        const size_t nameBytes = ::strnlen(entry->d_name, sizeof(entry->d_name));
        if (nameBytes == sizeof(entry->d_name))
            return error(std::errc::filename_too_long);
        if ((nameBytes == 1 && entry->d_name[0] == '.') ||
            (nameBytes == 2 && entry->d_name[0] == '.' && entry->d_name[1] == '.'))
            continue;
        const size_t slash = absoluteSpelling.back() == '/' ? 0 : 1;
        // Check subtraction before addition, and every quota before allocating
        // either the entry path or another vector element.
        if (absoluteSpelling.size() > maxPathBytes - slash ||
            nameBytes > maxPathBytes - slash - absoluteSpelling.size())
            return error(std::errc::filename_too_long);
        const size_t pathBytes = absoluteSpelling.size() + slash + nameBytes;
        if (result.entries.size() >= maxEntries ||
            pathBytes > maxStringBytes - stringBytes)
            return error(std::errc::file_too_large);
        std::string path = absoluteSpelling;
        if (slash)
            path.push_back('/');
        path.append(entry->d_name, nameBytes);
        result.entries.emplace_back(std::move(path), entryType(entry->d_type));
        stringBytes += pathBytes;
    }
    struct stat after{};
    if (::fstat(fd, &after) == -1)
        return nativeError();
    const auto finalPath = physicalPath(fd, maxPathBytes);
    if (!sameObjectState(before, after) || !finalPath ||
        *finalPath != result.physicalPath)
        return error(std::errc::state_not_recoverable);
    return result;
#else
    // A path-based FindFirstFile would reopen a possibly replaced target.
    // Until handle-based enumeration is implemented, fail explicitly.
    (void)policy;
    (void)maxEntries;
    (void)maxStringBytes;
    return error(std::errc::not_supported);
#endif
}

} // namespace topo::check
