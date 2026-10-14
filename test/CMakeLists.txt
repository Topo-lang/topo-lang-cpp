# Lang type tests (header-only, no LLVM)
add_subdirectory(isolated-build)

add_executable(topo-lang-cpp-tests
    unit/WholeExternalTypeTest.cpp
    unit/RuntimeTypesTest.cpp
    unit/CppStubGeneratorTest.cpp
    unit/CppAnalysisProviderTest.cpp
    unit/CppAuditTest.cpp
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

# Recording/pinning input helpers are exercised independently while the audit
# provider still uses its existing cache protocol. Directory reads on Windows
# are explicitly unsupported by the helper; that platform needs its own
# handle-based enumeration and runtime verification before enabling this suite.
if(TARGET topo-extract-cpp AND (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
add_executable(topo-cpp-audit-filesystem-tests
    unit/CppAuditFileSystemTest.cpp
    unit/CppAuditNativeFileTest.cpp
    ../topo-check/extractor/CppAuditFileSystem.cpp
    ../topo-check/extractor/CppAuditNativeFile.cpp)
target_include_directories(topo-cpp-audit-filesystem-tests SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
# Link the exported component target: bundled LLVM distributions need not
# provide the optional monolithic LLVM target or a library named libLLVM.
target_link_libraries(topo-cpp-audit-filesystem-tests PRIVATE LLVMSupport GTest::gtest_main)
target_compile_features(topo-cpp-audit-filesystem-tests PRIVATE cxx_std_17)
# This target subclasses LLVM VFS and buffer interfaces. Its RTTI mode must
# match the selected LLVM package or the derived classes reference absent
# base typeinfo in distributions built with LLVM_ENABLE_RTTI=OFF.
if(NOT LLVM_ENABLE_RTTI)
    if(MSVC)
        target_compile_options(topo-cpp-audit-filesystem-tests PRIVATE /GR-)
    else()
        target_compile_options(topo-cpp-audit-filesystem-tests PRIVATE -fno-rtti)
    endif()
endif()
gtest_discover_tests(topo-cpp-audit-filesystem-tests
    TEST_PREFIX "cpp-audit-filesystem." DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain;lang-cpp;audit" TIMEOUT 60)
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

# Semantic audit tests require libclang; the compile-model and preprocessor
# cases above remain available in the LLVM-free language SDK build.
if(TARGET topo-extract-cpp)
find_package(Clang REQUIRED CONFIG)
add_executable(topo-cpp-audit-semantic-tests
    unit/CppAuditTest.cpp
    unit/CppAuditDriverTest.cpp
    ../topo-check/extractor/CppAuditDriver.cpp
    ../topo-check/extractor/CppAuditExtractor.cpp)
target_compile_definitions(topo-cpp-audit-semantic-tests PRIVATE TOPO_CPP_AUDIT_LIBCLANG=1)
target_include_directories(topo-cpp-audit-semantic-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../topo-check
    ${CMAKE_CURRENT_SOURCE_DIR}/../topo-check/extractor)
target_include_directories(topo-cpp-audit-semantic-tests SYSTEM PRIVATE
    ${CLANG_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_link_libraries(topo-cpp-audit-semantic-tests PRIVATE
    TopoCppAnalysis topo::core::TopoDistribution libclang clangDriver GTest::gtest_main)
add_dependencies(topo-cpp-audit-semantic-tests topo-extract-cpp)
# Keep the real extractor adjacent to this test executable so the provider's
# installed-tool lookup is exercised without a project-local override.
set_target_properties(topo-cpp-audit-semantic-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/topo-check-audit-tests")
add_custom_command(TARGET topo-cpp-audit-semantic-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:topo-extract-cpp> $<TARGET_FILE_DIR:topo-cpp-audit-semantic-tests>)
gtest_discover_tests(topo-cpp-audit-semantic-tests TEST_PREFIX "cpp-audit-semantic." DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain;lang-cpp;audit" TIMEOUT 60)
endif()

# These internal frontend controls use the matching Clang package and the
# macOS SDK. Other platforms require their own standard-library input grants.
# The helper remains separate from the CLI until its observation protocol and
# worker resource limits are connected.
if(TARGET topo-extract-cpp AND APPLE AND TARGET clang-cpp)
set(_audit_frontend_resource "${CLANG_INSTALL_PREFIX}/lib/clang/${LLVM_VERSION_MAJOR}")
set(_audit_frontend_compiler "${CLANG_INSTALL_PREFIX}/bin/clang++")
set(_audit_frontend_sdk "${CMAKE_OSX_SYSROOT}")
if(NOT IS_DIRECTORY "${_audit_frontend_sdk}")
    find_program(_audit_xcrun xcrun REQUIRED)
    execute_process(COMMAND "${_audit_xcrun}" --show-sdk-path
        OUTPUT_VARIABLE _audit_frontend_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
endif()
add_executable(topo-cpp-audit-frontend-tests
    unit/CppAuditFrontendTest.cpp
    ../topo-check/extractor/CppAuditFrontend.cpp
    ../topo-check/extractor/CppAuditDriver.cpp
    ../topo-check/extractor/CppAuditFileSystem.cpp
    ../topo-check/extractor/CppAuditNativeFile.cpp)
target_include_directories(topo-cpp-audit-frontend-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../topo-check/extractor)
target_include_directories(topo-cpp-audit-frontend-tests SYSTEM PRIVATE
    ${CLANG_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_definitions(topo-cpp-audit-frontend-tests PRIVATE
    "TOPO_FRONTEND_TEST_COMPILER=\"${_audit_frontend_compiler}\""
    "TOPO_FRONTEND_TEST_RESOURCE=\"${_audit_frontend_resource}\""
    "TOPO_FRONTEND_TEST_SDK=\"${_audit_frontend_sdk}\""
    TOPO_FRONTEND_TEST_RECORDING_FS=1)
target_compile_features(topo-cpp-audit-frontend-tests PRIVATE cxx_std_17)
target_link_libraries(topo-cpp-audit-frontend-tests PRIVATE
    libclang clang-cpp LLVM nlohmann_json::nlohmann_json GTest::gtest_main)
gtest_discover_tests(topo-cpp-audit-frontend-tests
    TEST_PREFIX "cpp-audit-frontend." DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain;lang-cpp;audit" TIMEOUT 60)
endif()
