#include "CppPlugin.h"

#include "ClangdBridge.h"
#include "CppAnalysisProvider.h"
#include "CppCheckRunner.h"
#include "CppEmitter.h"

namespace topo::lang {

// -----------------------------------------------------------------------
// EmitterFactory
// -----------------------------------------------------------------------

class CppPlugin::CppEmitterFactory : public EmitterFactory {
public:
    std::unique_ptr<transpile::Emitter> createEmitter() override {
        return std::make_unique<transpile::CppEmitter>();
    }
    std::string fileExtension() const override { return ".cpp"; }
};

// -----------------------------------------------------------------------
// BuildDriverFactory
// -----------------------------------------------------------------------

class CppPlugin::CppBuildDriverFactory : public BuildDriverFactory {
public:
    std::string backendToolName() const override { return "topo-build-llvm-cpp"; }
    std::string extractorToolName() const override { return "topo-extract-cpp"; }
};

// -----------------------------------------------------------------------
// CppPlugin
// -----------------------------------------------------------------------

CppPlugin::CppPlugin()
    : emitterFactory_(std::make_unique<CppEmitterFactory>()),
      buildDriverFactory_(std::make_unique<CppBuildDriverFactory>()) {}

HostLanguage CppPlugin::language() const { return HostLanguage::Cpp; }

std::unique_ptr<check::LanguageAnalysisProvider> CppPlugin::createAnalysisProvider() {
    return check::createCppAnalysisProvider();
}

EmitterFactory* CppPlugin::emitterFactory() { return emitterFactory_.get(); }
BuildDriverFactory* CppPlugin::buildDriverFactory() { return buildDriverFactory_.get(); }
InitTemplateProvider* CppPlugin::initTemplateProvider() { return &initProvider_; }

std::unique_ptr<lsp::LSPBridge> CppPlugin::createLSPBridge() {
    return std::make_unique<lsp::ClangdBridge>();
}

std::unique_ptr<CheckRunnerBase> CppPlugin::createCheckRunner() {
    return std::make_unique<CppCheckRunner>();
}

void registerCppPlugin() {
    registerPlugin(std::make_unique<CppPlugin>());
}

} // namespace topo::lang
