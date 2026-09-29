#include <AYCombat/SceneIntegration.h>
#include <AYScene.h>
#include <AYScene/SceneManager.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/SceneEvents.h>
namespace ayt::combat {
struct CombatSceneBinding::Impl {
    ayt::event::EventBus* bus=nullptr;
    ayt::scene::SceneManager* scenes=nullptr;
    std::vector<ayt::event::ConnectionId> subscriptions;
    std::optional<Diagnostic> error;
    Result<void> installCurrent() {
        for(auto* scene:{scenes->current(),scenes->edit(),scenes->play()})if(scene) {
            auto result=installCombat(scene->world());if(!result){error=result.error();return std::unexpected(result.error());}
        }
        return {};
    }
};
CombatSceneBinding::CombatSceneBinding()=default;
CombatSceneBinding::~CombatSceneBinding()=default;
Result<void> CombatSceneBinding::bind(ayt::event::EventBus& bus,ayt::scene::SceneManager& scenes) {
    disconnect();auto state=std::make_shared<Impl>();state->bus=&bus;state->scenes=&scenes;_impl=state;
    const std::weak_ptr<Impl> weak=state;
    auto install=[weak](const auto&){if(auto owner=weak.lock())(void)owner->installCurrent();};
    state->subscriptions.push_back(bus.subscribe<ayt::event::SceneCurrentChangedEvent>(install));
    state->subscriptions.push_back(bus.subscribe<ayt::event::SceneBeginPlayEvent>(install));
    state->subscriptions.push_back(bus.subscribe<ayt::event::SceneEndPlayEvent>(install));
    return state->installCurrent();
}
void CombatSceneBinding::disconnect() {
    if(!_impl)return;for(auto id:_impl->subscriptions)_impl->bus->unsubscribe(id);_impl.reset();
}
std::optional<Diagnostic> CombatSceneBinding::lastError() const {return _impl?_impl->error:std::optional<Diagnostic>{};}
} // namespace ayt::combat
