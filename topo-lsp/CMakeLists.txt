add_library(TopoCppLSP
    ClangdBridge.cpp
)
add_library(topo::lang-cpp::TopoCppLSP ALIAS TopoCppLSP)

target_include_directories(TopoCppLSP PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-lsp>
)
target_link_libraries(TopoCppLSP PUBLIC topo::core::TopoLSPBridge)
# TOPO_LLVM_BINDIR was a monorepo-only define for the bundled clangd path.
# Standalone topo-lang-cpp expects clangd on PATH; ClangdBridge falls back
# to "clangd" when the define is empty.
target_compile_definitions(TopoCppLSP PRIVATE TOPO_LLVM_BINDIR="")
topo_set_compiler_flags(TopoCppLSP)

install(TARGETS TopoCppLSP
    EXPORT topo-lang-cppTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-lsp
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-lsp
    FILES_MATCHING PATTERN "*.h"
)
