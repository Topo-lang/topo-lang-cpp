add_library(TopoCppAnalysis
    CppAnalysisProvider.cpp
    extract/CppCallEdgeExtractor.cpp
    extract/CppCallSiteExtractor.cpp
    extract/CppImportExtractor.cpp
    extract/CppLSPCallSiteExtractor.cpp
    extract/CppLSPImportExtractor.cpp
    extract/CppLSPSymbolExtractor.cpp
    extract/CppSymbolAccessExtractor.cpp
    extract/CppSymbolExtractor.cpp    # regex-based; kept for escape detection & L1 fallback
    catalog/CppPreprocessorScanner.cpp
    catalog/CppSafePatterns.cpp
    catalog/CppSafetyAnalyzer.cpp
    catalog/CppUnsafeCatalog.cpp
    stub/CppStubGenerator.cpp
)

find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp REQUIRED)

add_library(topo::lang-cpp::TopoCppAnalysis ALIAS TopoCppAnalysis)

target_include_directories(TopoCppAnalysis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-check/analysis>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/extract
        ${CMAKE_CURRENT_SOURCE_DIR}/catalog
        ${CMAKE_CURRENT_SOURCE_DIR}/stub
        ${TOMLPLUSPLUS_INCLUDE_DIR}
)
# topo-core's include path comes via topo::core::TopoCheck PUBLIC
# propagation. The previous PRIVATE topo-lsp/lib include is a dead path
# (LSPBridge.h moved to topo-core in phase-1 D2).
target_link_libraries(TopoCppAnalysis
    PUBLIC topo::core::TopoCheck TopoCppLSP
    PRIVATE topo::core::TopoPlatform)
# TOPO_SOURCE_DIR is used by CppSafePatterns.cpp for source-tree catalog
# discovery (with runtime fallback if not present). In standalone mode
# we point it at the topo-lang-cpp source root so the legacy path
# fallback still works during development; install-time consumers
# should override via -DTOPO_SOURCE_DIR=<your-checkout-root>. The runtime
# code returns false safely when the catalog can't be located.
target_compile_definitions(TopoCppAnalysis PRIVATE
    TOPO_SOURCE_DIR="${CMAKE_SOURCE_DIR}/.."
)
topo_set_compiler_flags(TopoCppAnalysis)

install(TARGETS TopoCppAnalysis
    EXPORT topo-lang-cppTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-check/analysis
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-check/analysis
    FILES_MATCHING PATTERN "*.h"
)
