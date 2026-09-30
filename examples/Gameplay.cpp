#include <AYCombat/EntityIntegration.h>
#include <AYEntity.h>
#include <AYEntity/EntityModule.h>
#include <iostream>
#include <stdexcept>
using namespace ayt::combat;
template<class T> T require(Result<T> result) {
    if(!result)throw std::runtime_error(result.error().message);return std::move(*result);
}
void require(Result<void> result) {if(!result)throw std::runtime_error(result.error().message);}
int main() {
    auto& world=ayt::entity::World::processWorld();
    try {
        world.initialize();auto& registry=ayt::entity::ComponentRegistry::instance();
        (void)ayt::entity::registerEntityCoreComponents(registry);
        require(registerCombatGameplayComponents(registry));registry.seal();
        auto& combat=*require(installCombat(world));
        auto schema=ayt::stats::Schema::compile({
            {.name="vitality",.shape=ayt::stats::Shape::Pool,.current=100,.capacity=100},
            {.name="mana",.shape=ayt::stats::Shape::Pool,.current=50,.capacity=50}
        });
        if(!schema)throw std::runtime_error(schema.error().message);
        auto* a=world.createEntity();auto* b=world.createEntity();
        auto attacker=world.getEntityHandle(a->getId()),target=world.getEntityHandle(b->getId());
        ActorProfile profile{*schema,{ayt::stats::statId("vitality")},1};
        require(combat.configureActor(attacker,profile));profile.team=2;require(combat.configureActor(target,profile));
        require(combat.setRelation(1,2,Relation::Hostile));
        combat.setOnOutcome([](const auto& result){std::cout<<"actual damage: "<<result.actual<<"\n";});

        auto result=require(combat.damage(attacker,target,30));
        auto cost=require(combat.decrease(attacker,"mana",10));
        InteractionPolicy hostileDrain{InteractionIntent::Harmful,mask(AllowedTarget::Hostile)};
        auto drained=require(combat.interactAttribute(attacker,target,"mana",ayt::stats::AttributeOp::Decrease,5,hostileDrain));
        std::cout<<"health: "<<result.remaining<<", mana: "<<cost.after<<"\n";
        std::cout<<"permitted enemy mana change: "<<(drained.attribute?drained.attribute->actual:0)<<"\n";
        world.fixedUpdate(1.0f/60); // Normal engine Sim drives notification delivery.
        world.shutdown();return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<"\n";world.shutdown();return 1;}
}
