# topo-app-static-cpp: the clang-AST C++ compile-time static-analysis
# front-end — the upgrade of the regex MVP that ships header-only as
# topo-lang-cpp/runtime/include/topo/app_static.h.
#
# Structure mirrors the delivered Rust front-end
# (topo-lang-rust/topo-build/static-frontend, the `topo-app-static-rust`
# binary). Unlike the Rust front-end it REUSES the existing
# app_emit.h / app_readback.h / app_check.h headers and the
# topo::app::Graph model — only the clang-AST-JSON -> Graph analyze
# stage and the CLI are new code here.
#
# AST acquisition (planning doc task-1 decision): clang is invoked as a
# subprocess (`clang -Xclang -ast-dump=json`), resolved at run time via
# topo::platform::resolveLLVMTool("clang") — no LibTooling / libclang
# link dependency. The clang binary itself is therefore not a build
# dependency; only its builtin-header resource-dir is located here so
# the subprocess can be pointed at it (the same handling topo-extract-cpp
# uses).

add_executable(topo-app-static-cpp
    main.cpp
    AppStaticAnalyze.cpp
)

target_include_directories(topo-app-static-cpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    # the topo::app::Graph model + app_emit.h (reused, not rewritten)
    ${PROJECT_SOURCE_DIR}/runtime/include
    # topo::platform::resolveLLVMTool / Process — provided transitively by
    # topo::core::TopoPlatform's PUBLIC include propagation
)

# topo::core::TopoPlatform carries resolveLLVMTool() + the process
# abstraction. nlohmann_json parses the clang AST dump — same JSON
# library topo-core's BackendProtocol already uses.
target_link_libraries(topo-app-static-cpp PRIVATE
    topo::core::TopoPlatform
    nlohmann_json::nlohmann_json
)

# Locate the bundled clang resource-dir (lib/clang/<MAJOR>) so the
# clang -ast-dump=json subprocess can find <stddef.h> and friends.
# Same discovery topo-extract-cpp's CMake performs.
set(_APP_STATIC_RESOURCE_DIR "")
if(TOPO_LLVM_BINDIR)
    get_filename_component(_LLVM_ROOT "${TOPO_LLVM_BINDIR}" DIRECTORY)
    file(GLOB _RD_CAND LIST_DIRECTORIES true "${_LLVM_ROOT}/lib/clang/*")
    foreach(_d ${_RD_CAND})
        if(IS_DIRECTORY "${_d}" AND EXISTS "${_d}/include")
            set(_APP_STATIC_RESOURCE_DIR "${_d}")
        endif()
    endforeach()
endif()
if(NOT _APP_STATIC_RESOURCE_DIR STREQUAL "")
    message(STATUS "topo-app-static-cpp: clang resource-dir = ${_APP_STATIC_RESOURCE_DIR}")
    target_compile_definitions(topo-app-static-cpp PRIVATE
        "TOPO_APP_STATIC_CPP_RESOURCE_DIR=\"${_APP_STATIC_RESOURCE_DIR}\"")
else()
    message(STATUS "topo-app-static-cpp: clang resource-dir not located; "
                   "the subprocess relies on clang's default search paths")
endif()

topo_set_compiler_flags(topo-app-static-cpp)
