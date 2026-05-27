# topo-lang-cpp/topo-profile — C++ span-emitter fixture
# for the topo-profile demo.
#
# The fixture binary lives here (mirroring the topo-debug fixture layout)
# because its only consumer is the topo-profile e2e tests, and its
# breakpoint-free nature makes it host-language-agnostic in spirit but
# cpp-built in practice.

if(NOT TOPO_ENABLE_LLVM)
    return()
endif()

# Profile fixture is only meaningful when monorepo e2e tests consume it.
if(TOPO_LANG_CPP_BUILD_TEST_FIXTURES)
    add_subdirectory(test)
endif()
