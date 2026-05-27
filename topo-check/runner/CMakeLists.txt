add_library(TopoCppCheck STATIC CppCheckRunner.cpp)
add_library(topo::lang-cpp::TopoCppCheck ALIAS TopoCppCheck)

target_include_directories(TopoCppCheck PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-check/runner>
)
target_link_libraries(TopoCppCheck PUBLIC topo::lang::TopoLang)
topo_set_compiler_flags(TopoCppCheck)

install(TARGETS TopoCppCheck
    EXPORT topo-lang-cppTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-check/runner
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-check/runner
    FILES_MATCHING PATTERN "*.h"
)
