add_library(TopoCppTranspile
    CppEmitter.cpp
)
add_library(topo::lang-cpp::TopoCppTranspile ALIAS TopoCppTranspile)

target_include_directories(TopoCppTranspile PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-transpile>
)
# topo-core/include comes via topo::core::TopoTranspile's PUBLIC propagation.
target_link_libraries(TopoCppTranspile PUBLIC topo::core::TopoTranspile)
topo_set_compiler_flags(TopoCppTranspile)

install(TARGETS TopoCppTranspile
    EXPORT topo-lang-cppTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-transpile
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lang-cpp/topo-transpile
    FILES_MATCHING PATTERN "*.h"
)
