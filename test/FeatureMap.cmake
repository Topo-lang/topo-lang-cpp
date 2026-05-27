# topo-lang-cpp/test/FeatureMap.cmake — Feature dimension labels for C++ lang tests.
# Extracted from the root test/FeatureMap.cmake during repo-split preparation.
#
# Contains labels for CppStubGenerator (topo-lang-cpp-tests) and NinjaGen
# (topo-lang-cpp-integration-tests) test suites.
#
# Mechanism: set_tests_properties applies feature labels at CTest time via
# TEST_INCLUDE_FILES. CTest silently ignores non-existent test names.

# --- CppStubGenerator ---
set_tests_properties(
    CppStubGenerator.FindSimpleFunction
    CppStubGenerator.FindVoidFunction
    CppStubGenerator.FindFunctionWithQualifiers
    CppStubGenerator.FindFunctionWithTrailingReturn
    CppStubGenerator.FindFunctionAmongMultiple
    CppStubGenerator.DoesNotMatchSubstring
    CppStubGenerator.DoesNotMatchDeclaration
    CppStubGenerator.FindFunctionNotFound
    CppStubGenerator.MatchingBraceSimple
    CppStubGenerator.MatchingBraceNested
    CppStubGenerator.MatchingBraceWithStrings
    CppStubGenerator.MatchingBraceWithComments
    CppStubGenerator.MatchingBraceBlockComment
    CppStubGenerator.MatchingBraceUnmatched
    CppStubGenerator.IsVoidReturnTrue
    CppStubGenerator.IsVoidReturnFalse
    PROPERTIES LABELS "toolchain;checker")
set_tests_properties(
    CppStubGeneratorFileTest.StubAndRestore
    CppStubGeneratorFileTest.StubVoidFunction
    CppStubGeneratorFileTest.StubFunctionNotFound
    CppStubGeneratorFileTest.StubNestedBraces
    PROPERTIES LABELS "toolchain;checker")

# --- Ninja Build Generation ---
set_tests_properties(
    NinjaGenTest.FlattenSimpleName
    NinjaGenTest.FlattenNestedPath
    NinjaGenTest.FlattenAbsolutePath
    NinjaGenTest.FlattenAvoidsSameNameConflict
    NinjaGenTest.GenerateBasicNinjaFile
    NinjaGenTest.GenerateWithIncludes
    NinjaGenTest.GenerateWithDefines
    NinjaGenTest.GenerateSharedLibFlagsUnix
    NinjaGenTest.ResolveNinjaReturnsString
    PROPERTIES LABELS "toolchain;build")
