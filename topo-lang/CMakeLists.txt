add_library(TopoCppPlugin
    CppPlugin.cpp
)
add_library(topo::lang-cpp::TopoCppPlugin ALIAS TopoCppPlugin)
target_include_directories(TopoCppPlugin
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo/lang-cpp/plugin>
    PRIVATE ${PROJECT_SOURCE_DIR}/topo-check/analysis
    PRIVATE ${PROJECT_SOURCE_DIR}/topo-check/runner
    PRIVATE ${PROJECT_SOURCE_DIR}/topo-transpile
    PRIVATE ${PROJECT_SOURCE_DIR}/topo-lsp
)
target_link_libraries(TopoCppPlugin PUBLIC
    topo::lang::TopoLang
    TopoCppInit
    TopoCppAnalysis
    TopoCppCheck
    TopoCppTranspile
    TopoCppLSP
)
topo_set_compiler_flags(TopoCppPlugin)

install(TARGETS TopoCppPlugin
    EXPORT topo-lang-cppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES CppPlugin.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo/lang-cpp/plugin)
