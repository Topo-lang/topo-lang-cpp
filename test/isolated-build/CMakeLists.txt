if(NOT TARGET TopoCppDriver)
    return()
endif()

add_executable(topo-cpp-isolated-build-tests ../unit/IsolatedCppBuildTest.cpp ../unit/CppCommandModelTest.cpp ../unit/CppEvidenceCacheTest.cpp)
target_link_libraries(topo-cpp-isolated-build-tests PRIVATE TopoCppDriver GTest::gtest_main)
topo_set_compiler_flags(topo-cpp-isolated-build-tests)

if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    add_executable(topo-cpp-sandbox-probe SandboxProbe.cpp)
    target_compile_features(topo-cpp-sandbox-probe PRIVATE cxx_std_17)
    target_compile_definitions(topo-cpp-isolated-build-tests PRIVATE
        TOPO_SANDBOX_PROBE_BINARY="$<TARGET_FILE:topo-cpp-sandbox-probe>")
    add_dependencies(topo-cpp-isolated-build-tests topo-cpp-sandbox-probe)
endif()

# The real compiler/sandbox cases require explicitly selected trusted host
# tools: TOPO_TEST_ISOLATED_CLANG and TOPO_TEST_ISOLATED_LD. Other platforms
# exercise rejection cases and report the unsupported integration cases.
gtest_discover_tests(topo-cpp-isolated-build-tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "integration\;lang-cpp\;evidence\;isolated-build" TIMEOUT 90)
