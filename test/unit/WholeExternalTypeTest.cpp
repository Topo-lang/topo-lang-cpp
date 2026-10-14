#include "analysis/extract/CppCallSiteExtractor.h"
#include "topo/Basic/Diagnostic.h"
#include "topo/Check/ContainmentCheck.h"
#include "topo/Lexer/Lexer.h"
#include "topo/Parser/Parser.h"
#include "topo/Sema/SemanticAnalyzer.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
using namespace topo;
using namespace topo::check;

TEST(CppWholeExternalType, QualifiedOwnershipDoesNotAuthorizeOtherClass) {
    auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures/spec/external_type";
    std::ifstream input(fixture / "minimal.topo"); std::ostringstream contents; contents << input.rdbuf();
    const auto source = contents.str(); ASSERT_FALSE(source.empty());
    DiagnosticEngine diag; Lexer lexer(source, "minimal.topo", diag); Parser parser(lexer, diag);
    auto ast = parser.parseTopoFile(); ASSERT_NE(ast, nullptr);
    SemanticAnalyzer sema(diag); auto symbols = sema.analyze(*ast); ASSERT_FALSE(diag.hasErrors());
    CppCallSiteExtractor extractor;
    auto sites = extractor.extractCallSites((fixture / "app.cpp").string());
    std::vector<DetectedCallSite> own, unrelated;
    std::ostringstream observed;
    for (const auto& site : sites) observed << site.callerQualifiedName << " -> " << site.calleePattern << "\n";
    for (const auto& site : sites) {
        if (site.callerQualifiedName == "app::Log::hidden") own.push_back(site);
        if (site.callerQualifiedName == "app::Other::hidden") unrelated.push_back(site);
    }
    ASSERT_FALSE(own.empty()) << observed.str(); ASSERT_FALSE(unrelated.empty()) << observed.str();
    ContainmentConfig config; config.mode = FeatureMode::Force; config.strictIdentity = true;
    CheckResult ownResult, otherResult;
    checkContainment(symbols, {}, own, config, ownResult, "::");
    checkContainment(symbols, {}, unrelated, config, otherResult, "::");
    // L1 Cpp retains the full owner.
    EXPECT_TRUE(ownResult.passed());
    EXPECT_FALSE(otherResult.passed());
}
