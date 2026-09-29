#include <AYTest.h>
#include <AYCombat/SceneIntegration.h>
#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/IEngineHost.h>
#include <AYEntity/EntityComponentModule.h>
#include <AYEntity/EntityRuntimeModule.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYEntity.h>
#include <AYScene.h>
#include <AYScene/SceneManager.h>
using namespace ayt::combat;
TEST_SUITE(CombatHostTests)
TEST_CASE(startup_needs_only_combat_nodes_and_scene_lifecycle_installs_stats_automatically) {
    auto& host=ayt::app::defaultEngineHost();ayt::app::bindBuiltinHostServices(host);
    ayt::app::EngineModuleRuntime runtime(host);
    CHECK_TRUE(runtime.modules().emplace<ayt::entity::EntityComponentModule>().succeeded());
    CHECK_TRUE(runtime.modules().emplace<ayt::entity::EntityRuntimeModule>().succeeded());
    CHECK_TRUE(runtime.modules().emplace<CombatIntegrationModule>().succeeded());
    CHECK_TRUE(runtime.modules().emplace<CombatHostIntegrationModule>().succeeded());
    CHECK_TRUE(runtime.prepare().succeeded());runtime.context().componentRegistry().seal();
    CHECK_TRUE(runtime.install().succeeded());
    ayt::scene::Scene active(ayt::scene::SceneMode::Edit,"CombatHostActive");host.scenes()->setCurrent(&active);
    auto* combat=dynamic_cast<CombatSystem*>(active.world().findSystemByName("CombatSystem"));CHECK_NOT_NULL(combat);
    CHECK_NOT_NULL(active.world().findSystemByName("StatsSystem"));
    auto* entity=active.world().createEntity();auto handle=active.world().getEntityHandle(entity->getId());
    auto schema=ayt::stats::Schema::compile({{.name="life",.shape=ayt::stats::Shape::Pool,.current=10,.capacity=10}}).value();
    CHECK_TRUE(combat->configureActor(handle,{schema,{ayt::stats::statId("life")},1}).has_value());
    CHECK_TRUE(combat->damage(handle,handle,10)->kind==OutcomeKind::Rejected);
    CHECK_TRUE(combat->attributes().decrease(handle,"life",10).has_value());
    CHECK_TRUE(entity->getComponent<CombatComponent>()->life()->phase()==LifePhase::Dead);
    host.scenes()->setCurrent(nullptr);runtime.shutdown();
    ayt::scene::Scene later(ayt::scene::SceneMode::Edit,"AfterCombatShutdown");host.scenes()->setCurrent(&later);
    CHECK_TRUE(later.world().findSystemByName("CombatSystem")==nullptr);host.scenes()->setCurrent(nullptr);
}
TEST_SUITE_END
