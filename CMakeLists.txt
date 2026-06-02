cmake_minimum_required(VERSION 3.20)

project(topo-lang-cpp
    VERSION 0.1.0
    DESCRIPTION "Topo C++ language support — runtime headers, check, build/transpile, LSP bridge, plugin"
    HOMEPAGE_URL "https://github.com/topo-lang/topo-lang-cpp"
    LANGUAGES C CXX
)

# topo-lang-cpp: C++ language support (user headers + compilation driver)
# Provides C++ user-facing headers (parallel.h, jit.h, tuple.h, etc.) and
# the C++ compilation driver (CppDriver). C ABI runtime libraries live in
# topo-llvm/runtime/.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TopoLangCppCompilerFlags)

# ── Upstream Topo packages ────────────────────────────
if(NOT TARGET topo::core::TopoBasic)
    find_package(topo-core CONFIG REQUIRED)
endif()
if(NOT TARGET topo::lang::TopoLang)
    find_package(topo-lang CONFIG REQUIRED)
endif()

# ── External dependencies ─────────────────────────────
find_package(nlohmann_json CONFIG REQUIRED)

# Header-only interface library for C++ runtime headers
add_library(topo-lang-cpp-headers INTERFACE)
add_library(topo::lang-cpp::topo-lang-cpp-headers ALIAS topo-lang-cpp-headers)
target_include_directories(topo-lang-cpp-headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
install(TARGETS topo-lang-cpp-headers EXPORT topo-lang-cppTargets)
install(DIRECTORY runtime/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# ── Optional LLVM-dependent toolchain layer ───────────
# topo-build/CppDriver, topo-debug, topo-profile fixtures, and the
# topo-check/extractor (LibTooling-based C++ extractor) depend on the
# LLVM/Clang dev libraries. Default OFF; consumers that need them set
# -DTOPO_LANG_CPP_ENABLE_LLVM=ON and arrange Clang/LLDB findable.
option(TOPO_LANG_CPP_ENABLE_LLVM
    "Enable LLVM-dependent components (CppDriver, debug adapter, profile fixture, LibTooling extractor)"
    OFF)

if(TOPO_LANG_CPP_ENABLE_LLVM)
    find_package(LLVM CONFIG REQUIRED)
    message(STATUS "topo-lang-cpp: LLVM ${LLVM_VERSION} enabled")
    # TopoCppDriver consumes topo-llvm headers (topo/Backend/BackendTypes.h)
    # via the imported topo::llvm::topo-llvm-headers INTERFACE target.
    if(NOT TARGET topo::llvm::TopoBackend)
        find_package(topo-llvm CONFIG REQUIRED)
    endif()
    # Existing TOPO_ENABLE_LLVM-gated subdir CMakeLists rely on this name
    # to fire their content.
    set(TOPO_ENABLE_LLVM ON)
endif()

# Test-only fixture builds (tiny_matrix / project_simple / ...) consumed by
# monorepo e2e tests in topo-core/test. Default OFF in standalone mode.
option(TOPO_LANG_CPP_BUILD_TEST_FIXTURES
    "Build C++ debug/profile test fixture binaries (only useful inside monorepo e2e)"
    OFF)

# ── Subdirs (most are zero-LLVM; LLVM-gated ones self-skip) ───────────
# Order: analysis/runner first because other subdirs include their headers.
add_subdirectory(topo-check/analysis)
add_subdirectory(topo-check/runner)
add_subdirectory(topo-transpile)
add_subdirectory(topo-lsp)
add_subdirectory(topo-init)
# These three self-gate on TOPO_ENABLE_LLVM:
add_subdirectory(topo-build)
add_subdirectory(topo-debug)
add_subdirectory(topo-profile)
# extractor: uses Clang LibTooling → gated on TOPO_LANG_CPP_ENABLE_LLVM
if(TOPO_LANG_CPP_ENABLE_LLVM)
    add_subdirectory(topo-check/extractor)
endif()
# Plugin aggregator — depends on the above libs. Some PRIVATE includes
# point into LLVM-gated subdirs, so we gate it on TOPO_LANG_CPP_ENABLE_LLVM
# for the pilot (covering it requires the build/debug subdirs being live).
if(TOPO_LANG_CPP_ENABLE_LLVM)
    add_subdirectory(topo-lang)
endif()

# ── Install / export ──────────────────────────────────
install(EXPORT topo-lang-cppTargets
    FILE topo-lang-cppTargets.cmake
    NAMESPACE topo::lang-cpp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-lang-cpp
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/topo-lang-cppConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/topo-lang-cppConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-lang-cpp
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/topo-lang-cppConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/topo-lang-cppConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/topo-lang-cppConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-lang-cpp
)

# ── Tests (self-gated) ────────────────────────────────
# The header-only unit suite (topo-lang-cpp-tests, ~163 cases linking the
# zero-LLVM TopoCppAnalysis + TopoCppTranspile + TopoCppLSP targets) used to
# run ONLY in the meta full-stack build (via the meta-root's explicit
# add_subdirectory(topo-lang-cpp/test)) — the per-repo public CI built
# topo-lang-cpp with no tests and passed green ("No tests were found!!!").
# This self-gate runs the suite in the standalone zero-LLVM CI. It is distinct
# from TOPO_LANG_CPP_BUILD_TEST_FIXTURES (monorepo e2e fixtures) above, and
# stays DORMANT in the meta build (TOPO_LANG_CPP_BUILD_TESTS defaults OFF
# there, so the meta's explicit add remains the single source — no double-add).
# test/CMakeLists.txt self-skips its LLVM/plugin-gated suites, so standalone
# scope yields exactly the header-only cases.
option(TOPO_LANG_CPP_BUILD_TESTS "Build topo-lang-cpp tests" OFF)
if(TOPO_LANG_CPP_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR TOPO_META_DRIVING_TESTS))
    enable_testing()
    if(NOT TARGET GTest::gtest_main)
        include(FetchContent)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
    set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
    include(GoogleTest)
    add_subdirectory(test)
endif()
