# Lang type tests (header-only, no LLVM)
add_executable(topo-lang-cpp-tests
    unit/RuntimeTypesTest.cpp
    unit/CppStubGeneratorTest.cpp
    unit/CppAnalysisProviderTest.cpp
    unit/CppSymbolExtractorTest.cpp
    unit/CppSymbolAccessExtractorTest.cpp
    unit/CppExtractorTest.cpp
    unit/CppEmitterStdlibTest.cpp
    unit/PreprocessorScannerTempFileRegressionTest.cpp
    unit/AppStaticRegexTest.cpp
)
# TopoCppAnalysis PUBLIC-propagates TopoCppLSP → TopoLSPBridge → TopoPlatform,
# so explicit TopoPlatform was the duplicate.
target_link_libraries(topo-lang-cpp-tests PRIVATE
    TopoCppAnalysis TopoCppTranspile
    GTest::gtest_main
)
# Anchored on this test dir's parent (the topo-lang-cpp source root) so the
# paths resolve in BOTH the meta build (PROJECT_SOURCE_DIR = meta root) and the
# standalone build (PROJECT_SOURCE_DIR = clone root). The previous
# ${PROJECT_SOURCE_DIR}/topo-lang-cpp/... form only resolved inside the meta.
target_include_directories(topo-lang-cpp-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include     # topo/array.h, span.h, ...
    ${CMAKE_CURRENT_SOURCE_DIR}/../topo-check          # analysis/stub, analysis/extract
    ${CMAKE_CURRENT_SOURCE_DIR}/../topo-transpile      # CppEmitter.h
)
# The topo-llvm C-ABI runtime headers live in the sibling topo-llvm repo, only
# present in the meta layout; the standalone clone neither has nor needs them
# for this header-only SDK suite (array/span/slot/table/view are self-contained).
if(EXISTS "${PROJECT_SOURCE_DIR}/topo-llvm/runtime/include")
    target_include_directories(topo-lang-cpp-tests PRIVATE
        ${PROJECT_SOURCE_DIR}/topo-llvm/runtime/include)
endif()
topo_set_compiler_flags(topo-lang-cpp-tests)
# AppStaticRegexTest includes app_static.h → app.h, whose Field<"name", T>
# NTTP (fixed_string) + constexpr std::string require C++20. The other suites
# here are C++17-clean and compile unchanged under C++20; raise the floor so
# the app-model header (and its regex front-end) can be unit-tested.
target_compile_features(topo-lang-cpp-tests PRIVATE cxx_std_20)
gtest_discover_tests(topo-lang-cpp-tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "toolchain\;lang-cpp\;stdlib" TIMEOUT 60)

# NinjaGen integration test (needs LLVM tool paths for clang resolution)
if(TOPO_ENABLE_LLVM)
add_executable(topo-lang-cpp-integration-tests
    integration/NinjaGenTest.cpp
)
target_link_libraries(topo-lang-cpp-integration-tests PRIVATE
    # topo::core::TopoBuildConfig (namespaced imported target), NOT the bare
    # `TopoBuildConfig`: standalone, topo-core is consumed via find_package and
    # exposes only the ALIAS; a bare name becomes `-lTopoBuildConfig` and fails
    # ("cannot find -lTopoBuildConfig" — it is an INTERFACE lib with no artifact).
    TopoCppDriver topo::core::TopoBuildConfig
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
target_include_directories(topo-lang-cpp-integration-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/topo-lang-cpp/topo-build     # for NinjaGen.h
)
topo_set_llvm_flags(topo-lang-cpp-integration-tests)
set_property(TARGET topo-lang-cpp-integration-tests PROPERTY JOB_POOL_LINK llvm_link)
gtest_discover_tests(topo-lang-cpp-integration-tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "integration\;lang-cpp" TIMEOUT 60)
endif()

# Checker E2E suite — lives in check-e2e/ as a self-gating, idempotent file
# so a meta-style super-build can re-add it after its topo-cli sibling
# configures; see check-e2e/CMakeLists.txt for the gating rationale. This
# inline add covers the standalone path (installed topo-cli on
# CMAKE_PREFIX_PATH) and self-skips with a STATUS message otherwise.
add_subdirectory(check-e2e)

# Extractor fidelity tests: validate topo-extract-cpp output against
# golden fixtures for symbol-extraction parity.
# Spawns topo-extract-cpp subprocess against golden fixtures; the extractor
# itself requires libclang via the bundled LLVM, so this test is gated on
# TOPO_ENABLE_LLVM. The test driver uses only stdlib + posix_spawn/CreateProcess
# for subprocess management (see the header comment in
# CppExtractorFidelityTest.cpp for rationale — PipedProcess is intended for
# JSON-RPC bidirectional framing and would deadlock on one-shot extraction).
if(TOPO_ENABLE_LLVM)
if(TARGET topo-extract-cpp)
add_executable(topo-cpp-extractor-fidelity-tests
    unit/CppExtractorFidelityTest.cpp
)
target_link_libraries(topo-cpp-extractor-fidelity-tests PRIVATE
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
target_compile_definitions(topo-cpp-extractor-fidelity-tests PRIVATE
    TOPO_CPP_FIDELITY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/extractor_fidelity"
    TOPO_EXTRACT_CPP_BINARY="$<TARGET_FILE:topo-extract-cpp>"
)
add_dependencies(topo-cpp-extractor-fidelity-tests topo-extract-cpp)
topo_set_compiler_flags(topo-cpp-extractor-fidelity-tests)
gtest_discover_tests(topo-cpp-extractor-fidelity-tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain\;lang-cpp\;extractor-fidelity" TIMEOUT 60
    ENVIRONMENT "TOPO_EXTRACT_ROOT=${PROJECT_SOURCE_DIR}")
else()
message(STATUS
    "topo-lang-cpp: skipping topo-cpp-extractor-fidelity-tests — "
    "target 'topo-extract-cpp' not in scope "
    "(topo-lang-cpp opted out via TOPO_LANG_CPP_ENABLE_LLVM=OFF; "
    "the extractor subdir at topo-check/extractor/ was not added).")
endif()
endif()

# Feature dimension labels
set_property(DIRECTORY APPEND PROPERTY
    TEST_INCLUDE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/FeatureMap.cmake")
