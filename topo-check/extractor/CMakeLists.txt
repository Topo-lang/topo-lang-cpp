# topo-extract-cpp: libclang-based C++ function body extractor
# Subprocess tool called by topo-transpile to extract TranspileModel JSON.
# Requires LLVM/Clang (only built when TOPO_ENABLE_LLVM is ON).

if(NOT TOPO_ENABLE_LLVM)
    return()
endif()

# libclang comes from the same LLVM distribution selected at the project
# root (see CMakeLists.txt — prefers Homebrew's llvm@<MAJOR>, falls back to
# topo-llvm/llvm-dev/). Deriving Clang_DIR from LLVM_INSTALL_PREFIX keeps
# the two paired: otherwise bundled ClangConfig.cmake would call
# `find_dependency(LLVM)` and pull bundled LLVM in behind a brew LLVM_DIR.
if(NOT DEFINED Clang_DIR AND NOT DEFINED CACHE{Clang_DIR})
    if(DEFINED LLVM_INSTALL_PREFIX
            AND EXISTS "${LLVM_INSTALL_PREFIX}/lib/cmake/clang/ClangConfig.cmake")
        set(Clang_DIR "${LLVM_INSTALL_PREFIX}/lib/cmake/clang"
            CACHE PATH "Clang from the LLVM distribution selected by root CMakeLists")
    endif()
endif()

find_package(Clang REQUIRED CONFIG)
message(STATUS "topo-extract-cpp: using libclang from ${CLANG_INSTALL_PREFIX}")

# Resolve the bundled clang resource-dir (lib/clang/<MAJOR>). libclang needs
# this passed via -resource-dir so it can find builtin headers (<stddef.h>,
# <stdint.h>, intrinsics, etc.). Without it, source that pulls in any standard
# header transitively fails to parse.
set(_TOPO_RESOURCE_DIR "")
file(GLOB _RD_CAND LIST_DIRECTORIES true "${CLANG_INSTALL_PREFIX}/lib/clang/*")
foreach(_d ${_RD_CAND})
    if(IS_DIRECTORY "${_d}" AND EXISTS "${_d}/include")
        set(_TOPO_RESOURCE_DIR "${_d}")
    endif()
endforeach()
if(_TOPO_RESOURCE_DIR STREQUAL "")
    message(WARNING "topo-extract-cpp: could not locate bundled clang resource-dir under ${CLANG_INSTALL_PREFIX}/lib/clang")
else()
    message(STATUS "topo-extract-cpp: bundled resource-dir = ${_TOPO_RESOURCE_DIR}")
endif()

add_executable(topo-extract-cpp main.cpp)

target_include_directories(topo-extract-cpp PRIVATE
    ${CLANG_INCLUDE_DIRS}
)

target_compile_definitions(topo-extract-cpp PRIVATE
    "TOPO_EXTRACT_CPP_RESOURCE_DIR=\"${_TOPO_RESOURCE_DIR}\""
)

target_link_libraries(topo-extract-cpp PRIVATE
    libclang
    topo::core::TopoTranspile
    nlohmann_json::nlohmann_json
)

topo_set_compiler_flags(topo-extract-cpp)

install(TARGETS topo-extract-cpp
    EXPORT topo-lang-cppTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
