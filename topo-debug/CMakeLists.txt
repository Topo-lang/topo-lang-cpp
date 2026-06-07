# topo-debug-cpp — Extract adapter for C++ binaries.
#
# Speaks the debug wire protocol on stdout/stdin, driving a target
# binary through liblldb. Mirrors topo-debug-mock-adapter's role but with
# real-debugger backing instead of in-memory fixtures.
#
# liblldb does not ship its own LLDBConfig.cmake in the bundled LLVM, so
# we discover the headers and dylib by path (mirroring how topo-extract-cpp
# falls back when ClangConfig is the entry point).

if(NOT TOPO_ENABLE_LLVM)
    return()
endif()

# Derive LLDB paths from the LLVM distribution selected by find_package(LLVM).
# LLVM_INSTALL_PREFIX is set by LLVMConfig.cmake; bundled topo-llvm/llvm-dev/
# and brew/system LLVM share the same layout.
set(_LLDB_INCLUDE_DIR "${LLVM_INSTALL_PREFIX}/include")
set(_LLDB_LIB_DIR "${LLVM_INSTALL_PREFIX}/lib")

# Discover the liblldb library file (Linux: .so, macOS: .dylib, Windows: .dll).
find_library(LLDB_LIBRARY
    NAMES lldb
    PATHS "${_LLDB_LIB_DIR}"
    NO_DEFAULT_PATH
)

if(NOT EXISTS "${_LLDB_INCLUDE_DIR}/lldb/API/SBDebugger.h" OR NOT LLDB_LIBRARY)
    message(WARNING
        "topo-debug-cpp: liblldb not found under ${_LLDB_LIB_DIR} "
        "or headers missing at ${_LLDB_INCLUDE_DIR}/lldb/API/SBDebugger.h. "
        "Skipping topo-debug-cpp build; tests will be disabled.")
    return()
endif()

message(STATUS "topo-debug-cpp: using liblldb at ${LLDB_LIBRARY}")

# Shared STATIC library — also linked by topo-debug-rust (topo-lang-rust/
# topo-debug). Exported via topo-lang-cppTargets so downstream packages can
# pick it up as topo::lang-cpp::TopoDebugAdapter. STATIC (not OBJECT) so the
# install/EXPORT pipeline can carry it across repos.
add_library(TopoDebugAdapter STATIC adapter.cpp)
add_library(topo::lang-cpp::TopoDebugAdapter ALIAS TopoDebugAdapter)
target_include_directories(TopoDebugAdapter PUBLIC
    $<BUILD_INTERFACE:${_LLDB_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(TopoDebugAdapter PUBLIC
    topo::core::TopoDebug
    "${LLDB_LIBRARY}"
    nlohmann_json::nlohmann_json
)
topo_set_compiler_flags(TopoDebugAdapter)

add_executable(topo-debug-cpp $<TARGET_OBJECTS:TopoDebugAdapter>)
target_link_libraries(topo-debug-cpp PRIVATE TopoDebugAdapter)
topo_set_compiler_flags(topo-debug-cpp)

# RPATH so the binary finds liblldb.dylib at runtime. BUILD_RPATH keeps the
# build-tree binary pointing at the build host's LLVM lib dir (dev runs work
# unchanged). INSTALL_RPATH is relocation-safe: @loader_path/../lib resolves
# liblldb relative to the installed binary (so a packaged tree that ships its
# own lib/ works), with the build host's lib dir kept only as a dev fallback.
set_target_properties(topo-debug-cpp PROPERTIES
    BUILD_RPATH "${_LLDB_LIB_DIR}"
    INSTALL_RPATH "@loader_path/../lib;${_LLDB_LIB_DIR}"
)

install(TARGETS TopoDebugAdapter topo-debug-cpp
    EXPORT topo-lang-cppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Test fixtures (tiny_matrix / tiny_vector / project_simple / ...) are only
# meaningful inside the monorepo where topo-core/test consumes them as
# $<TARGET_FILE:tiny_matrix>. Skip in standalone mode.
if(TOPO_LANG_CPP_BUILD_TEST_FIXTURES)
    add_subdirectory(test)
endif()
