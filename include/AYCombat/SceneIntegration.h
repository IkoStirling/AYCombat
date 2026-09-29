#pragma once
#include <AYCombat/EntityIntegration.h>
namespace ayt::event { class EventBus; }
namespace ayt::scene { class SceneManager; }
namespace ayt::combat {
/// Host-owned weak subscriptions acquire the complete gameplay facade for current/edit/play Worlds.
class CombatSceneBinding {
public:
    CombatSceneBinding();
    ~CombatSceneBinding();
    Result<void> bind(ayt::event::EventBus&,ayt::scene::SceneManager&);
    void disconnect();
    std::optional<Diagnostic> lastError() const;
private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
/// Optional standard Application startup node; disconnect before the Host destroys Scene/EventBus.
class CombatHostIntegrationModule final : public ayt::module::IModule {
public:
    CombatHostIntegrationModule();
    const ayt::module::ModuleDescriptor& descriptor() const noexcept override { return _descriptor; }
    ayt::module::ModuleResult install(ayt::module::IModuleContext&) override;
    void shutdown(ayt::module::IModuleContext&) noexcept override;
private:
    ayt::module::ModuleDescriptor _descriptor;
    CombatSceneBinding _binding;
};
} // namespace ayt::combat
