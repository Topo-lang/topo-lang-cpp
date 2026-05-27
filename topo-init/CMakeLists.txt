# embed the native-lldb formatter into the init provider
# so `topo init` lays down a self-contained scaffold. Read the canonical
# script and configure_file it into a raw-string TU. CONFIGURE_DEPENDS-style
# re-run is handled by listing the .py as a configure dependency.
set(_CPP_LLDB_FORMATTER
    "${CMAKE_CURRENT_SOURCE_DIR}/../topo-debug/lldb_formatter.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_CPP_LLDB_FORMATTER}")
file(READ "${_CPP_LLDB_FORMATTER}" TOPO_CPP_LLDB_FORMATTER_SOURCE)
set(_CPP_LLDB_ASSET "${CMAKE_CURRENT_BINARY_DIR}/CppLldbFormatterAsset.cpp")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/CppLldbFormatterAsset.cpp.in"
    "${_CPP_LLDB_ASSET}"
    @ONLY)

add_library(TopoCppInit STATIC
    CppInitTemplateProvider.cpp
    "${_CPP_LLDB_ASSET}")
add_library(topo::lang-cpp::TopoCppInit ALIAS TopoCppInit)

target_include_directories(TopoCppInit PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-init>
)
target_link_libraries(TopoCppInit PUBLIC topo::lang::TopoLang)
topo_set_compiler_flags(TopoCppInit)

install(TARGETS TopoCppInit
    EXPORT topo-lang-cppTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-init
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-init
    FILES_MATCHING PATTERN "*.h"
)
