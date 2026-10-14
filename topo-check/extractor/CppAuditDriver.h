#ifndef TOPO_CPP_AUDIT_DRIVER_H
#define TOPO_CPP_AUDIT_DRIVER_H

#include <string>

namespace topo::check {
struct CppCompileUnit;
// Apply only to the expanded, sanitized compile-model arguments. Returns a
// rejection before either Clang frontend entry point can construct a Driver.
// This does not replace the analysis worker's child-process denial.
std::string cppAuditDriverRejection(const CppCompileUnit& unit);
}
#endif
