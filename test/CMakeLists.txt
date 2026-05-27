# Lang type tests (header-only, no LLVM)
add_executable(topo-lang-cpp-tests
    unit/RuntimeTypesTest.cpp
    unit/CppStubGeneratorTest.cpp
    unit/CppSymbolExtractorTest.cpp
    unit/CppExtractorTest.cpp
    unit/CppEmitterStdlibTest.cpp
    unit/PreprocessorScannerTempFileRegressionTest.cpp
)
# TopoCppAnalysis PUBLIC-propagates TopoCppLSP → TopoLSPBridge → TopoPlatform,
# so explicit TopoPlatform was the duplicate.
target_link_libraries(topo-lang-cpp-tests PRIVATE
    TopoCppAnalysis TopoCppTranspile
    GTest::gtest_main
)
target_include_directories(topo-lang-cpp-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/topo-lang-cpp/runtime/include
    ${PROJECT_SOURCE_DIR}/topo-lang-cpp/topo-check   # for analysis/stub/CppStubGenerator.h
    ${PROJECT_SOURCE_DIR}/topo-lang-cpp/topo-transpile # for CppEmitter.h
    ${PROJECT_SOURCE_DIR}/topo-llvm/runtime/include  # for C ABI headers used by SDK types
)
topo_set_compiler_flags(topo-lang-cpp-tests)
gtest_discover_tests(topo-lang-cpp-tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "toolchain;lang-cpp;stdlib" TIMEOUT 60)

# NinjaGen integration test (needs LLVM tool paths for clang resolution)
if(TOPO_ENABLE_LLVM)
add_executable(topo-lang-cpp-integration-tests
    integration/NinjaGenTest.cpp
)
target_link_libraries(topo-lang-cpp-integration-tests PRIVATE
    TopoCppDriver TopoBuildConfig
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
target_include_directories(topo-lang-cpp-integration-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/topo-lang-cpp/topo-build     # for NinjaGen.h
)
topo_set_llvm_flags(topo-lang-cpp-integration-tests)
set_property(TARGET topo-lang-cpp-integration-tests PROPERTY JOB_POOL_LINK llvm_link)
gtest_discover_tests(topo-lang-cpp-integration-tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "integration;lang-cpp" TIMEOUT 60)
endif()

# Checker E2E tests (completeness + containment + purity + visibility + stage-isolation)
add_executable(topo-lang-cpp-check-tests
    e2e/TestMain.cpp
    e2e/CppCompletenessTest.cpp
    e2e/CppContainmentTest.cpp
    e2e/CppImportPathTest.cpp
    e2e/CppPurityTest.cpp
    e2e/CppVisibilityTest.cpp
    e2e/CppStageIsolationTest.cpp
)
target_link_libraries(topo-lang-cpp-check-tests PRIVATE
    TopoCheckRunner TopoCppPlugin
    GTest::gtest
)
target_compile_definitions(topo-lang-cpp-check-tests PRIVATE
    TOPO_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/check"
)
topo_set_compiler_flags(topo-lang-cpp-check-tests)
gtest_discover_tests(topo-lang-cpp-check-tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "toolchain;checker;lang-cpp" TIMEOUT 60)

# Extractor fidelity tests (M4.2)
# Spawns topo-extract-cpp subprocess against golden fixtures; the extractor
# itself requires libclang via the bundled LLVM, so this test is gated on
# TOPO_ENABLE_LLVM. The test driver uses only stdlib + posix_spawn/CreateProcess
# for subprocess management (see the header comment in
# CppExtractorFidelityTest.cpp for rationale — PipedProcess is intended for
# JSON-RPC bidirectional framing and would deadlock on one-shot extraction).
if(TOPO_ENABLE_LLVM)
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
    PROPERTIES LABELS "toolchain;lang-cpp;extractor-fidelity" TIMEOUT 60
    ENVIRONMENT "TOPO_EXTRACT_ROOT=${PROJECT_SOURCE_DIR}")
endif()

# Feature dimension labels
set_property(DIRECTORY APPEND PROPERTY
    TEST_INCLUDE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/FeatureMap.cmake")
