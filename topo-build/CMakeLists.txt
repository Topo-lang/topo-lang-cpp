if(TOPO_ENABLE_LLVM)
    add_library(TopoCppDriver STATIC
        CppDriver.cpp
        NinjaGen.cpp
    )
    add_library(topo::lang-cpp::TopoCppDriver ALIAS TopoCppDriver)

    target_include_directories(TopoCppDriver PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo/lang-cpp/build>
    )
    target_link_libraries(TopoCppDriver PUBLIC
        topo::core::TopoPlatform
        topo::core::TopoBuildConfig
        topo::core::TopoBuildLib
        topo::llvm::topo-llvm-headers
    )

    topo_set_llvm_flags(TopoCppDriver)

    install(TARGETS TopoCppDriver
        EXPORT topo-lang-cppTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(FILES CppDriver.h NinjaGen.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo/lang-cpp/build)
endif()

# topo-app compile-time static-analysis front-end (clang-AST). Builds
# regardless of TOPO_ENABLE_LLVM: it links only TopoPlatform +
# nlohmann_json and resolves the clang binary at run time (subprocess),
# so it has no build-time LLVM dependency.
add_subdirectory(static-frontend)
