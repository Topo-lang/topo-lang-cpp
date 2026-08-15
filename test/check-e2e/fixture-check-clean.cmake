# fixture-check-clean.cmake — per-fixture check-clean ctest guard for the
# topo-debug project fixtures (project_simple / project_multi / project_chart
# / project_formatter).
#
# Runs `topo-check --project <fixture>` over a build-tree copy of each
# fixture and asserts BOTH rc=0 and warning-free stdout
# (FAIL_REGULAR_EXPRESSION "WARNING"). rc=0 alone is not enough: a
# completeness warning (e.g. a declared class with no host counterpart — see
# the fixtures' [completeness] ignore_patterns) keeps the exit code at 0, so
# an rc-only gate passes while the fixture is not actually check-clean and
# stays misleading for anyone using it as a reference.
#
# Included from test/check-e2e/CMakeLists.txt — deliberately, because that
# file is the one topo-lang-cpp test surface the meta full-stack build
# re-adds AFTER topo-cli configures (its topo-lang-cpp-check-e2e-late hook):
# it is the only configure point in this package where the in-tree
# `topo-check` target exists. The inline pass (standalone, or the meta's
# first pass) self-skips below — topo-check is a topo-cli target and
# topo-cli configures after topo-lang-cpp.
#
# The guard stages its own copies of the fixture sources into the build
# tree at configure time (the same staging pattern topo-debug/test uses) so
# topo-check's .topo-check-cache never lands in the source checkout and
# never races the topo-debug e2e's fixture dirs. It therefore does not
# depend on TOPO_LANG_CPP_BUILD_TEST_FIXTURES — check-cleanliness of the
# fixture sources is meaningful whether or not the fixture binaries build.

if(NOT TARGET topo-check)
    message(STATUS "topo-lang-cpp tests: debug-fixture check-clean guard skipped "
                   "— in-tree topo-check not in scope (topo-cli configures "
                   "after this package; the meta re-adds check-e2e late).")
    return()
endif()

# Anchor the fixture sources on this file's location: test/check-e2e/ →
# test/ → package root, so both the inline and the meta-late include
# resolve them.
get_filename_component(_fc_test_root "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
get_filename_component(_fc_pkg_root "${_fc_test_root}" DIRECTORY)

set(_fc_stage "${CMAKE_CURRENT_BINARY_DIR}/debug-fixture-check")
set(_fc_fixtures project_simple project_multi project_chart project_formatter)
foreach(_fc_fixture IN LISTS _fc_fixtures)
    file(MAKE_DIRECTORY "${_fc_stage}/${_fc_fixture}")
    foreach(_fc_src Topo.toml main.topo main.cpp)
        file(COPY "${_fc_pkg_root}/topo-debug/test/fixtures/${_fc_fixture}/${_fc_src}"
             DESTINATION "${_fc_stage}/${_fc_fixture}")
    endforeach()
endforeach()

foreach(_fc_fixture IN LISTS _fc_fixtures)
    add_test(NAME check-clean.topo-debug-cpp.${_fc_fixture}
        COMMAND $<TARGET_FILE:topo-check> --project "${_fc_stage}/${_fc_fixture}")
    set_tests_properties(check-clean.topo-debug-cpp.${_fc_fixture} PROPERTIES
        WORKING_DIRECTORY "${_fc_stage}/${_fc_fixture}"
        # Warnings keep rc=0, so the exit-code gate alone cannot see them;
        # fail on any WARNING line to pin warning-clean fixtures.
        FAIL_REGULAR_EXPRESSION "WARNING"
        LABELS "check-clean;e2e;topo-debug-cpp;toolchain"
        TIMEOUT 60)
endforeach()
