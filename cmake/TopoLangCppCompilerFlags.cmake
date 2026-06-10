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

# Make an installed LLVM-linked Mach-O/ELF artifact relocatable. Usage:
#   topo_relocate_llvm_rpath(<target> <pattern> [DESTINATION <install-subdir>])
# `pattern` is the dylib basename stem to rewrite (libLLVM | libclang | liblldb).
# DESTINATION is the artifact's install subdir as given to install(TARGETS);
# default ${CMAKE_INSTALL_BINDIR} (executables). Pass the LIBDIR for an
# installed shared library — a lib/ artifact expects bundled dylibs NEXT to
# itself (rpath @loader_path / $ORIGIN) instead of in the sibling lib dir
# (@loader_path/../lib / $ORIGIN/../lib).
# macOS: prepends the relocation rpath (kept FIRST, ahead of any pre-set dev
# fallback and the build-host LLVM lib dir appended by
# CMAKE_INSTALL_RPATH_USE_LINK_PATH), rewrites the build-host-absolute
# lib<pattern> load command to @rpath/<basename> at install time, and
# canonicalizes a versioned Homebrew Cellar LLVM rpath to its stable /opt
# form (see cmake/RelocateLLVMDylib.cmake).
# Linux (ELF): prepends the $ORIGIN-relative runpath so a bundled libLLVM.so
# resolves relative to the artifact — no load-command rewrite needed, the
# dynamic linker resolves the DT_NEEDED soname through the runpath.
# No-op on Windows (DLL search has no rpath; PATH / app-dir rules apply).
# Call this AFTER the target's install(TARGETS) so the macOS rewrite runs on
# the installed copy.
function(topo_relocate_llvm_rpath target pattern)
    if(WIN32)
        return()
    endif()
    cmake_parse_arguments(_reloc "" "DESTINATION" "" ${ARGN})
    set(_dest "${_reloc_DESTINATION}")
    if(NOT _dest)
        set(_dest "${CMAKE_INSTALL_BINDIR}")
        if(NOT _dest)
            set(_dest "bin")
        endif()
    endif()
    # Artifacts installed into the lib dir find bundled dylibs NEXT to
    # themselves; anything else (bin/ tools) finds them in the sibling ../lib.
    if(_dest STREQUAL "${CMAKE_INSTALL_LIBDIR}" OR _dest STREQUAL "lib")
        set(_rpath_apple "@loader_path")
        set(_rpath_elf "\$ORIGIN")
    else()
        set(_rpath_apple "@loader_path/../lib")
        set(_rpath_elf "\$ORIGIN/../lib")
    endif()
    if(APPLE)
        set(_rpath_reloc "${_rpath_apple}")
    else()
        set(_rpath_reloc "${_rpath_elf}")
    endif()
    # PREPEND the relocation rpath so it stays first even when the target
    # already declares a build-host dev fallback in INSTALL_RPATH.
    get_target_property(_rpath_prev ${target} INSTALL_RPATH)
    if(_rpath_prev)
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "${_rpath_reloc};${_rpath_prev}")
    else()
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "${_rpath_reloc}")
    endif()
    if(NOT APPLE)
        return()
    endif()
    # Resolve the installed path the SAME way CMake's own install(TARGETS) does:
    # $ENV{DESTDIR} + (absolute DESTINATION as-is | CMAKE_INSTALL_PREFIX-joined),
    # both deferred to install time. Without the DESTDIR prefix a staged install
    # (DESTDIR=, distro/Homebrew packaging) writes to $DESTDIR$PREFIX/bin but this
    # script would look at $PREFIX/bin, miss the file, and silently skip the
    # rewrite — shipping a non-relocatable binary. $<TARGET_FILE_NAME:…> (not
    # the bare target name) so SHARED libraries resolve to lib<name>.dylib.
    if(IS_ABSOLUTE "${_dest}")
        set(_reloc_bin "\$ENV{DESTDIR}${_dest}/$<TARGET_FILE_NAME:${target}>")
    else()
        set(_reloc_bin "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_dest}/$<TARGET_FILE_NAME:${target}>")
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
