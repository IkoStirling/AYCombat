#include <AYCombat/SceneIntegration.h>
#include <AYApplication/EngineModuleContext.h>
namespace ayt::combat {
CombatHostIntegrationModule::CombatHostIntegrationModule():_descriptor{.id="AYCombat.HostIntegration",.displayName="Combat Host Scene lifecycle",.version="0.1.0",
    .dependencies={ayt::module::ModuleDependency::required("AYCombat.Integration")}}{}
ayt::module::ModuleResult CombatHostIntegrationModule::install(ayt::module::IModuleContext& context) {
    auto* engine=dynamic_cast<ayt::app::EngineModuleContext*>(&context);auto* scenes=engine?engine->host().scenes():nullptr;
    if(!engine || !scenes)return ayt::module::ModuleResult::failure(ayt::module::ModuleErrorCode::TypeRegistrationFailed,"Combat Host integration requires EngineModuleContext Scene service");
    auto result=_binding.bind(engine->host().eventBus(),*scenes);
    return result?ayt::module::ModuleResult::success():ayt::module::ModuleResult::failure(ayt::module::ModuleErrorCode::TypeRegistrationFailed,result.error().message);
}
void CombatHostIntegrationModule::shutdown(ayt::module::IModuleContext&) noexcept {try{_binding.disconnect();}catch(...){}}
} // namespace ayt::combat
