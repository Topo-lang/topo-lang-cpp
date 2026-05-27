# TopoLangCppCompilerFlags.cmake — standalone compiler-flag helper for topo-lang-cpp.

if(NOT WIN32)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(APPLE)
        set(CMAKE_MACOSX_RPATH ON)
    endif()
endif()

set(TOPO_LANG_CPP_SANITIZER "" CACHE STRING
    "Enable sanitizers (address, undefined, thread, memory)")

function(topo_lang_cpp_apply_sanitizer target)
    if(NOT TOPO_LANG_CPP_SANITIZER)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target}
            PRIVATE -fsanitize=${TOPO_LANG_CPP_SANITIZER} -fno-omit-frame-pointer)
        target_link_options(${target}
            PRIVATE -fsanitize=${TOPO_LANG_CPP_SANITIZER})
    endif()
endfunction()

function(topo_set_compiler_flags target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
    topo_lang_cpp_apply_sanitizer(${target})
endfunction()

function(topo_set_llvm_flags target)
    # Standalone topo-lang-cpp gates LLVM components on TOPO_LANG_CPP_ENABLE_LLVM
    # and uses find_package(LLVM CONFIG) at the top level; this helper just
    # wires the LLVM include dirs + definitions onto the target, mirroring
    # the monorepo helper's relevant parts. No RTTI tweak: most topo-lang-cpp
    # LLVM-touching code does not require -fno-rtti.
    topo_set_compiler_flags(${target})
    if(NOT TOPO_LANG_CPP_ENABLE_LLVM)
        return()
    endif()
    target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(${target} PRIVATE ${LLVM_DEFINITIONS})
endfunction()

if(NOT COMMAND topo_apply_std_pch)
    function(topo_apply_std_pch target)
        # PCH stub — no-op in standalone topo-lang-cpp.
    endfunction()
endif()
