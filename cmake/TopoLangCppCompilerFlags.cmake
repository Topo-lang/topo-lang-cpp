# TopoLangCppCompilerFlags.cmake — standalone compiler-flag helper for topo-lang-cpp.

# Directory of this module — used to locate sibling helper scripts
# (RelocateLLVMDylib.cmake) from inside install(CODE) at install time.
set(_TOPO_LANG_CPP_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

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

# Make an installed LLVM-linked tool relocatable on macOS. Adds an
# @loader_path/../lib rpath (so a dylib bundled next to the tool resolves) and
# rewrites the build-host-absolute libLLVM/libclang load command to
# @rpath/<basename> at install time (see cmake/RelocateLLVMDylib.cmake).
# `pattern` is the dylib basename stem to rewrite (libLLVM | libclang).
# No-op off APPLE (ELF resolves the soname through rpath already). Call this
# AFTER the target's install(TARGETS) so the rewrite runs on the installed copy.
function(topo_relocate_llvm_rpath target pattern)
    if(NOT APPLE)
        return()
    endif()
    # @loader_path/../lib first (relocation-safe); CMAKE_INSTALL_RPATH_USE_LINK_PATH
    # still appends the build-host LLVM lib dir afterwards as a dev fallback.
    set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "@loader_path/../lib")
    set(_bindir "${CMAKE_INSTALL_BINDIR}")
    if(NOT _bindir)
        set(_bindir "bin")
    endif()
    # Resolve the installed path the SAME way CMake's own install(TARGETS) does:
    # $ENV{DESTDIR} + (absolute DESTINATION as-is | CMAKE_INSTALL_PREFIX-joined),
    # both deferred to install time. Without the DESTDIR prefix a staged install
    # (DESTDIR=, distro/Homebrew packaging) writes to $DESTDIR$PREFIX/bin but this
    # script would look at $PREFIX/bin, miss the file, and silently skip the
    # rewrite — shipping a non-relocatable binary.
    if(IS_ABSOLUTE "${_bindir}")
        set(_reloc_bin "\$ENV{DESTDIR}${_bindir}/${target}")
    else()
        set(_reloc_bin "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_bindir}/${target}")
    endif()
    install(CODE
"set(TOPO_RELOC_BINARY \"${_reloc_bin}\")
set(TOPO_RELOC_PATTERN \"${pattern}\")
include(\"${_TOPO_LANG_CPP_CMAKE_DIR}/RelocateLLVMDylib.cmake\")")
endfunction()

if(NOT COMMAND topo_apply_std_pch)
    function(topo_apply_std_pch target)
        # PCH stub — no-op in standalone topo-lang-cpp.
    endfunction()
endif()
