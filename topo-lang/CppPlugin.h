#ifndef TOPO_LANG_CPPPLUGIN_H
#define TOPO_LANG_CPPPLUGIN_H

#include "topo/Lang/LanguagePlugin.h"
#include "topo/Lang/CheckRunnerBase.h"
#include "topo/Lang/EmitterFactory.h"
#include "topo/Lang/BuildDriverFactory.h"
#include "CppInitTemplateProvider.h"

namespace topo::lang {

class CppPlugin : public LanguagePlugin {
public:
    CppPlugin();

    HostLanguage language() const override;
    std::unique_ptr<check::LanguageAnalysisProvider> createAnalysisProvider() override;
    EmitterFactory* emitterFactory() override;
    BuildDriverFactory* buildDriverFactory() override;
    InitTemplateProvider* initTemplateProvider() override;
    std::unique_ptr<lsp::LSPBridge> createLSPBridge() override;
    std::unique_ptr<CheckRunnerBase> createCheckRunner() override;

private:
    class CppEmitterFactory;
    class CppBuildDriverFactory;
    std::unique_ptr<CppEmitterFactory> emitterFactory_;
    std::unique_ptr<CppBuildDriverFactory> buildDriverFactory_;
    CppInitTemplateProvider initProvider_;
};

/// Call once at startup to register the C++ plugin.
void registerCppPlugin();

} // namespace topo::lang

#endif // TOPO_LANG_CPPPLUGIN_H
