#include <AYTest.h>
#include <AYEntity.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYEntity/SceneSerializer.h>
#include <AYCombat/SceneIntegration.h>
#include <AYScene.h>
#include <AYScene/SceneManager.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/SceneEvents.h>
#include <stdexcept>
using namespace ayt::combat;
TEST_SUITE(CombatSceneTests)
TEST_CASE(each_scene_automatically_acquires_dependencies_and_disconnect_stops_installation) {
    auto& manager=ayt::scene::SceneManager::instance();auto& bus=ayt::event::EventBus::instance();
    ayt::scene::Scene first(ayt::scene::SceneMode::Edit,"CombatFirst"),second(ayt::scene::SceneMode::Play,"CombatSecond");
    CombatSceneBinding binding;CHECK_TRUE(binding.bind(bus,manager).has_value());
    manager.setCurrent(&first);bus.emit(ayt::event::SceneCurrentChangedEvent{&first});
    CHECK_NOT_NULL(first.world().findSystemByName("CombatSystem"));CHECK_NOT_NULL(first.world().findSystemByName("StatsSystem"));
    const auto count=first.world().systemCount();bus.emit(ayt::event::SceneCurrentChangedEvent{&first});CHECK_TRUE(first.world().systemCount()==count);
    manager.setCurrent(&second);bus.emit(ayt::event::SceneBeginPlayEvent{&second});
    auto* a=dynamic_cast<CombatSystem*>(first.world().findSystemByName("CombatSystem"));
    auto* b=dynamic_cast<CombatSystem*>(second.world().findSystemByName("CombatSystem"));
    CHECK_NOT_NULL(b);CHECK_TRUE(a->worldToken()!=b->worldToken());
    binding.disconnect();manager.setCurrent(nullptr);
    ayt::scene::Scene later(ayt::scene::SceneMode::Edit,"CombatLater");manager.setCurrent(&later);
    bus.emit(ayt::event::SceneCurrentChangedEvent{&later});CHECK_TRUE(later.world().findSystemByName("CombatSystem")==nullptr);
    manager.setCurrent(nullptr);
}
TEST_CASE(authored_life_mapping_roundtrips_and_initializes_without_business_rebinding) {
    auto& world=ayt::entity::World::processWorld();world.initialize();
    auto& registry=ayt::entity::ComponentRegistry::instance();
    if(!registry.isSealed()){CHECK_TRUE(registerCombatGameplayComponents(registry).has_value());registry.seal();}
    auto& combat=*installCombat(world).value();auto* actor=world.createEntity();actor->setName("SavedCombatActor");
    auto handle=world.getEntityHandle(actor->getId());
    auto schema=ayt::stats::Schema::compile({{.name="life",.shape=ayt::stats::Shape::Pool,.current=0,.capacity=100}}).value();
    CHECK_TRUE(combat.configureActor(handle,{schema,{ayt::stats::statId("life"),LifePhase::Downed,true},9}).has_value());
    const auto file=ayt::test::testTmpPath("combat-authored.ayscene");CHECK_TRUE(ayt::entity::saveScene(world,file));world.destroyEntity(actor);
    CHECK_TRUE(ayt::entity::loadScene(world,file));auto* loaded=world.findEntity("SavedCombatActor");CHECK_NOT_NULL(loaded);
    auto current=world.getEntityHandle(loaded->getId());
    CHECK_TRUE(bindCombatant(world,current).has_value());
    auto* component=loaded->getComponent<CombatComponent>();CHECK_NOT_NULL(component);
    if(!component || !component->life())throw std::runtime_error("serialized life mapping did not initialize");
    CHECK_TRUE(component->lifeResourceName=="life" && component->lifeResource==ayt::stats::statId("life"));
    CHECK_TRUE(component->life()->phase()==LifePhase::Downed && component->team==9);
    CHECK_TRUE(combat.increase(current,"life",10).has_value());
    CHECK_TRUE(component->life()->phase()==LifePhase::Alive);world.shutdown();
}
TEST_SUITE_END
