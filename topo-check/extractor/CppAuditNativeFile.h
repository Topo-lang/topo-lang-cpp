#ifndef TOPO_CPP_AUDIT_NATIVE_FILE_H
#define TOPO_CPP_AUDIT_NATIVE_FILE_H

#include <llvm/Support/VirtualFileSystem.h>
#include <memory>
#include <string>
#include <vector>

namespace topo::check {

// Created from trusted authorization, never from a cache witness. The caller
// supplies already resolved, absolute physical roots and exact file grants.
struct CppAuditReadPolicy {
    std::vector<std::string> roots;
    std::vector<std::string> files;
};

struct CppAuditNativeSnapshot {
    llvm::vfs::Status status;
    std::string physicalPath;
    std::shared_ptr<const std::string> bytes;
};

// Open without reading, resolve the opened object's physical path, authorize
// that object, then read a regular file with an allocation/read ceiling.
// Compare handle metadata before/after the read. This detects common mutations;
// it is not an atomic snapshot against adversarial in-place rewrite/restore.
llvm::ErrorOr<CppAuditNativeSnapshot> readCppAuditNativeFile(
    const std::string& absoluteSpelling, const CppAuditReadPolicy& policy,
    uint64_t maxBytes, size_t maxPathBytes);

struct CppAuditNativeDirectory {
    llvm::vfs::Status status;
    std::string physicalPath;
    std::vector<llvm::vfs::directory_entry> entries;
    std::error_code tailError;
};

// Enumerate the same authorized directory handle, with bounds checked before
// appending entries. Preserve native order and a terminal iteration error.
llvm::ErrorOr<CppAuditNativeDirectory> readCppAuditNativeDirectory(
    const std::string& absoluteSpelling, const CppAuditReadPolicy& policy,
    size_t maxEntries, size_t maxStringBytes, size_t maxPathBytes);

} // namespace topo::check
#endif
