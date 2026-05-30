# topo-lang-cpp runtime tests — the topo-app C++ runtime headers.
#
# These are header-only acceptance tests built with the project compiler;
# they need no GTest and no LLVM link. The static-front-end acceptance
# (test_app_static_slice.cpp) drives the freshly built toolchain binaries
# (topo / topo-check / topo-app-static-cpp) via three environment
# variables, set on the CTest test below from the real target locations.

# --- topo-app static-analysis front-end acceptance -----------------------
#
# Exercises the clang-AST front-end `topo-app-static-cpp` (primary path)
# and the regex MVP app_static.h (fallback) for vertical-slice parity,
# plus the five robustness cases the clang-AST upgrade exists to deliver.
add_executable(topo-app-static-slice-tests
    test_app_static_slice.cpp
)
target_include_directories(topo-app-static-slice-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include   # topo/app*.h
)
# app_readback.h / app_check.h now drive subprocesses through
# topo::platform::runProcessCapture (argv-style exec replacing
# shell-string popen) — the slice test links
# TopoPlatform so the new argv-style entry points and
# topo::platform::TempFile resolve. The runtime headers themselves
# stay self-contained for downstream consumers; only the link line of
# tests / drivers that actually USE app_readback's run_capture changes.
target_link_libraries(topo-app-static-slice-tests PRIVATE
    TopoPlatform
)
# The runtime header dir the clang-AST front-end must -I when it parses
# the analyzed C++ source (its `#include <topo/app.h>`).
target_compile_definitions(topo-app-static-slice-tests PRIVATE
    "TOPO_APP_RUNTIME_INCLUDE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../include\"")
topo_set_compiler_flags(topo-app-static-slice-tests)
# app.h's Field<fixed_string Name, T> is a C++20 class-type NTTP, so the
# test (which includes app.h as the runtime-bridge oracle) needs C++20 —
# override the C++17 default topo_set_compiler_flags applies.
set_target_properties(topo-app-static-slice-tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# The test spawns the fresh toolchain; make those binaries prerequisites
# so a plain `ctest` against this target always uses the current tree.
add_dependencies(topo-app-static-slice-tests
    topo topo-check topo-app-static-cpp)

add_test(NAME topo-app-static-slice
         COMMAND topo-app-static-slice-tests)
set_tests_properties(topo-app-static-slice PROPERTIES
    LABELS "toolchain;lang-cpp;topo-app"
    TIMEOUT 120
    ENVIRONMENT
        "TOPO_BIN=$<TARGET_FILE:topo>;TOPO_CHECK_BIN=$<TARGET_FILE:topo-check>;TOPO_APP_STATIC_CPP=$<TARGET_FILE:topo-app-static-cpp>")

# --- ast-dump consumption pin (cross-component contract test) -----------
#
# Catches drift in either direction:
#   * producer side -- `topo --ast-dump` text output regenerates the
#     pinned fixture byte for byte;
#   * consumer side -- app_readback.h's four regexes still parse that
#     output into the expected Graph shape.
#
# This is the round-trip contract test mandated by the
# cross-component-contracts principle (spec + schema + version + test).
add_executable(topo-lang-cpp-ast-dump-pin-tests
    test_app_readback_ast_dump_pin.cpp
)
target_include_directories(topo-lang-cpp-ast-dump-pin-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(topo-lang-cpp-ast-dump-pin-tests PRIVATE
    TopoPlatform)
topo_set_compiler_flags(topo-lang-cpp-ast-dump-pin-tests)
set_target_properties(topo-lang-cpp-ast-dump-pin-tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_dependencies(topo-lang-cpp-ast-dump-pin-tests topo topo-check)

add_test(NAME topo-lang-cpp-ast-dump-pin
         COMMAND topo-lang-cpp-ast-dump-pin-tests)
set_tests_properties(topo-lang-cpp-ast-dump-pin PROPERTIES
    LABELS "toolchain;lang-cpp;topo-app;contract"
    TIMEOUT 60
    ENVIRONMENT
        "TOPO_BIN=$<TARGET_FILE:topo>;TOPO_CHECK_BIN=$<TARGET_FILE:topo-check>;AST_DUMP_FIXTURE_TOPO=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ast-dump-roundtrip.topo;AST_DUMP_FIXTURE_EXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ast-dump-roundtrip.expected.txt")
