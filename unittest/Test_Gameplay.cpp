#include <AYTest.h>
#include <AYCombat/EntityIntegration.h>
#include <AYCombat/Action2DIntegration.h>
#include <AYEntity.h>
#include <AYEntity/EntityModule.h>
#include <array>
#include <stdexcept>
using namespace ayt::combat;
namespace {
using namespace ayt::stats;
struct GameplayFixture {
    ayt::entity::World& world=ayt::entity::World::processWorld();
    CombatSystem* combat=nullptr;
    ayt::entity::EntityHandle source,target;
    GameplayFixture(bool revive=false) {
        ayt::entity::World::setActiveWorld(nullptr);world.initialize();
        auto& registry=ayt::entity::ComponentRegistry::instance();
        if(!registry.isSealed()) {
            (void)ayt::entity::registerEntityCoreComponents(registry);
            if(!registerCombatGameplayComponents(registry))throw std::runtime_error("gameplay registration");registry.seal();
        }
        combat=installCombat(world).value();
        auto* a=world.createEntity();auto* b=world.createEntity();
        source=world.getEntityHandle(a->getId());target=world.getEntityHandle(b->getId());
        auto schema=Schema::compile({
            {.name="capacity",.mode=Mode::Advanced,.initial=100,.maximum=1000,.zones={{1,ZoneOp::Flat}}},
            {.name="vitality",.shape=Shape::Pool,.mode=Mode::Advanced,.current=100,.capacity=100,.capacityFrom=statId("capacity")},
            {.name="mana",.shape=Shape::Pool,.current=80,.capacity=100},
            {.name="rage",.shape=Shape::Pool,.current=0,.capacity=100},
            {.name="power",.mode=Mode::Advanced,.initial=20,.maximum=1000,.zones={{1,ZoneOp::PercentSum}}}
        }).value();
        ActorProfile profile{schema,{statId("vitality"),LifePhase::Dead,revive},10};
        if(!combat->configureActor(source,profile))throw std::runtime_error("source profile");
        profile.team=20;if(!combat->configureActor(target,profile))throw std::runtime_error("target profile");
        if(!combat->setRelation(10,20,Relation::Hostile))throw std::runtime_error("team relation");
    }
    ~GameplayFixture(){world.shutdown();}
    StatsComponent& stats(ayt::entity::EntityHandle handle){return *world.getEntityByHandle(handle)->getComponent<StatsComponent>();}
    CombatComponent& actor(){return *world.getEntityByHandle(target)->getComponent<CombatComponent>();}
    Value hp(){return stats(target).read(statId("vitality"))->current;}
    ayt::entity::EntityHandle additionalActor(std::uint64_t team) {
        auto* entity=world.createEntity();auto handle=world.getEntityHandle(entity->getId());
        ActorProfile profile{stats(source).state()->schema(),{statId("vitality")},team};
        if(!combat->configureActor(handle,profile))throw std::runtime_error("additional profile");
        return handle;
    }
    static Recipe scaled(std::string name,StatId target,AttributeOp op,Value rate,bool defaultRecipe=false) {
        auto plan=CalculationPlan::compile({{.id=1,.op=Op::Input,.binding=0},{.id=2,.op=Op::Constant,.value=rate},
            {.id=3,.op=Op::MultiplyRate,.a=1,.b=2}},3,1).value();
        return {std::move(name),target,op,std::move(plan),{{InputSource::Amount}},defaultRecipe};
    }
};
}
TEST_SUITE(CombatGameplayTests)
TEST_CASE(two_dimensional_action_hits_settle_once_in_action_order) {
    GameplayFixture f;
    ayt::ay2d::ActionStep2D step;
    step.active=true;
    step.cycle=1;
    step.hits={{11,0},{12,1}};
    auto resolve=[&](std::uint64_t id)->std::optional<ayt::entity::EntityHandle> {
        if(id==11)return f.target;
        return std::nullopt;
    };
    const auto settled=dispatchActionHits2D(*f.combat,f.source,step,resolve,25);
    CHECK_INT_EQ(settled.size(),2u);
    CHECK_TRUE(settled[0].result.has_value());
    CHECK_INT_EQ(settled[0].result->actual,25);
    CHECK_TRUE(!settled[1].result.has_value());
    CHECK_TRUE(settled[1].result.error().code==ayt::combat::Error::Missing);
    CHECK_INT_EQ(f.hp(),75);
}
TEST_CASE(exact_life_restoration_policy_is_authored_and_removal_is_atomic) {
    GameplayFixture f(true);
    CHECK_TRUE(f.combat->damage(f.source,f.target,20).has_value());
    CHECK_INT_EQ(f.hp(),80);
    EffectDefinition definition;
    definition.type=901;definition.key=901;
    definition.modifiers={{.rule=1,.target=statId("capacity"),.zone=1,.value=-99}};
    definition.lifeRestore=LifeRestoreMode::RestoreSnapshot;
    InteractionPolicy policy{InteractionIntent::Harmful,mask(AllowedTarget::Hostile)};
    auto applied=f.combat->applyOwnedEffect(f.source,f.target,definition,policy).value();
    CHECK_INT_EQ(f.hp(),1);
    CHECK_FALSE(f.combat->applyOwnedEffect(f.source,f.target,definition,policy).has_value());
    CHECK_TRUE(f.combat->damage(f.source,f.target,1).has_value());
    CHECK_INT_EQ(f.hp(),0);
    CHECK_TRUE(f.combat->removeOwnedEffect(applied.effectInstance).has_value());
    CHECK_INT_EQ(f.hp(),80);
    CHECK_INT_EQ(f.stats(f.target).read(statId("vitality"))->capacity,100);
    definition.lifeRestore=LifeRestoreMode::RestoreNetDelta;
    auto second=f.combat->applyOwnedEffect(f.source,f.target,definition,policy).value();
    CHECK_INT_EQ(f.hp(),1);
    CHECK_TRUE(f.combat->damage(f.source,f.target,1).has_value());
    CHECK_TRUE(f.combat->removeOwnedEffect(second.effectInstance).has_value());
    CHECK_INT_EQ(f.hp(),79);
}
TEST_CASE(simple_damage_heal_and_fixed_step_automatic_delivery) {
    GameplayFixture f;int outcomes=0,deaths=0;std::vector<Value> deliveredRemaining;
    f.combat->setOnOutcome([&](const ActionResult& result){++outcomes;deliveredRemaining.push_back(result.remaining);});
    f.combat->setOnLifeChanged([&](const auto& event){if(event.transition.after==LifePhase::Dead)++deaths;});
    auto first=f.combat->damage(f.source,f.target,30).value();
    CHECK_TRUE(first.calculated==30 && first.actual==30 && first.remaining==70 && first.excess==0 && f.hp()==70 && f.actor().life()->phase()==LifePhase::Alive);
    auto healing=f.combat->heal(f.target,f.target,10).value();
    CHECK_TRUE(healing.kind==OutcomeKind::Heal && healing.actual==10 && healing.remaining==80 && f.hp()==80);
    auto death=f.combat->damage(f.source,f.target,100).value();
    CHECK_TRUE(death.life.has_value() && death.actual==80 && death.remaining==0 && death.excess==20 && f.hp()==0 && f.actor().life()->phase()==LifePhase::Dead);
    CHECK_TRUE(outcomes==0 && deaths==0 && f.combat->hits().activeAttacks()==0 && f.combat->hits().retainedRequests()==0);
    f.world.fixedUpdate(.016f);CHECK_TRUE(outcomes==3 && deaths==1 && f.combat->outcomes().empty());
    CHECK_TRUE((deliveredRemaining==std::vector<Value>{70,80,0}));
    f.world.fixedUpdate(.016f);CHECK_TRUE(outcomes==3 && deaths==1);
}
TEST_CASE(default_recipe_explicit_override_and_current_source_target_sampling) {
    GameplayFixture f;
    auto defaultDamage=GameplayFixture::scaled("double_hit",statId("vitality"),AttributeOp::Decrease,20000,true);
    auto direct=GameplayFixture::scaled("half_hit",statId("vitality"),AttributeOp::Decrease,5000);
    auto bound=CalculationPlan::compile({{.id=1,.op=Op::Input,.binding=0},{.id=2,.op=Op::Input,.binding=1},
        {.id=3,.op=Op::Input,.binding=2},{.id=4,.op=Op::Add,.a=1,.b=2},{.id=5,.op=Op::Subtract,.a=4,.b=3}},5,3).value();
    Recipe formula{"bound",statId("vitality"),AttributeOp::Decrease,std::move(bound),
        {{InputSource::Amount},{InputSource::SourceStat,statId("power")},{InputSource::TargetStat,statId("power")}}};
    CHECK_TRUE(f.combat->attributes().setRecipes(RecipeBook::compile({defaultDamage,direct,formula}).value()).has_value());
    CHECK_TRUE(f.combat->damage(f.source,f.target,10)->calculated==20);
    CHECK_TRUE(f.combat->damage(f.source,f.target,10,"half_hit")->calculated==5);
    CHECK_TRUE(f.combat->attributes().increase(f.source,"power",5).has_value());
    CHECK_TRUE(f.combat->damage(f.source,f.target,10,"bound")->calculated==15 && f.hp()==60);
}
TEST_CASE(attribute_defaults_scalar_base_and_modifiers_share_the_same_owner) {
    GameplayFixture f;
    auto cost=GameplayFixture::scaled("mana_cost",statId("mana"),AttributeOp::Decrease,20000,true);
    auto half=GameplayFixture::scaled("cheap_cost",statId("mana"),AttributeOp::Decrease,5000);
    CHECK_TRUE(f.combat->attributes().setRecipes(RecipeBook::compile({cost,half}).value()).has_value());
    auto costResult=f.combat->decrease(f.target,"mana",10).value();
    CHECK_TRUE(costResult.before==80 && costResult.after==60 && costResult.actual==-20 && costResult.excess==0);
    CHECK_TRUE(f.combat->decrease(f.target,"mana",10,"cheap_cost")->actual==-5);
    CHECK_TRUE(f.combat->increase(f.target,"rage",10)->after==10);
    CHECK_TRUE(f.stats(f.target).read(statId("mana"))->current==55 && f.stats(f.target).read(statId("rage"))->current==10);
    Effect boost{1,1,UINT64_MAX,1,{{.handle=1,.rule=1,.target=statId("power"),.zone=1,.value=5000}}};
    CHECK_TRUE(f.stats(f.target).applyEffect(boost,EffectPolicy::Independent,0).has_value());
    CHECK_TRUE(f.combat->increase(f.target,"power",10)->after==30);
    auto row=f.stats(f.target).read(statId("power")).value();CHECK_TRUE(row.base==30 && row.effective==45);
    CHECK_TRUE(f.combat->setAttribute(f.target,"power",40)->actual==10);
    CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==60);
}
TEST_CASE(permitted_attribute_uses_intent_relation_selection_tag_and_capability) {
    GameplayFixture f;auto& target=f.actor();
    CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    InteractionPolicy policy{InteractionIntent::Beneficial,mask(AllowedTarget::Friendly),9,7,true};
    target.tags={7};const auto originalVersion=f.stats(f.target).version();
    auto unselected=f.combat->interactAttribute(f.source,f.target,"mana",AttributeOp::Decrease,10,policy).value();
    CHECK_TRUE(!unselected.decision.allowed && unselected.decision.denial==InteractionDenial::NotSelected && !unselected.attribute);
    target.tags.clear();
    auto untagged=f.combat->interactAttribute(f.source,f.target,"mana",AttributeOp::Decrease,10,policy,true).value();
    CHECK_TRUE(untagged.decision.denial==InteractionDenial::MissingTag);
    target.tags={7};target.immuneEffects={9};
    CHECK_TRUE(f.combat->interactAttribute(f.source,f.target,"mana",AttributeOp::Decrease,10,policy,true)->decision.denial==InteractionDenial::Immune);
    target.immuneEffects.clear();target.capabilities=mask(Capability::Damageable);
    CHECK_TRUE(f.combat->interactAttribute(f.source,f.target,"mana",AttributeOp::Decrease,10,policy,true)->decision.denial==InteractionDenial::MissingCapability);
    target.capabilities=mask(Capability::Damageable)|mask(Capability::Healable);
    CHECK_TRUE(f.stats(f.target).version()==originalVersion && f.stats(f.target).read(statId("mana"))->current==80);
    auto permitted=f.combat->interactAttribute(f.source,f.target,"mana",AttributeOp::Decrease,10,policy,true).value();
    CHECK_TRUE(permitted.decision.allowed && permitted.attribute && permitted.attribute->actual==-10);
    CHECK_TRUE(permitted.source.team==10 && permitted.target.object==f.target.id && f.stats(f.target).read(statId("mana"))->current==70);
    CHECK_TRUE(f.combat->setRelation(10,20,Relation::Hostile).has_value());
    const auto before=f.stats(f.target).version();
    CHECK_TRUE(f.combat->interactAttribute(f.source,f.target,"mana",AttributeOp::Decrease,10,policy,true)->decision.denial==InteractionDenial::RelationForbidden);
    CHECK_TRUE(f.stats(f.target).version()==before);
    policy.relationSnapshot=Relation::Friendly;
    auto captured=f.combat->interactAttribute(f.source,f.target,"mana",AttributeOp::Decrease,5,policy,true).value();
    CHECK_TRUE(captured.decision.allowed && captured.decision.relation==Relation::Friendly && captured.attribute->actual==-5);
    policy.relationSnapshot.reset();
    policy.intent=InteractionIntent::Harmful;policy.allowed=mask(AllowedTarget::Hostile);
    auto hostile=f.combat->interactAttribute(f.source,f.target,"rage",AttributeOp::Increase,10,policy,true).value();
    CHECK_TRUE(hostile.decision.allowed && hostile.attribute->actual==10 && f.stats(f.target).read(statId("rage"))->current==10);
    CHECK_TRUE(f.combat->hits().activeAttacks()==0 && f.combat->outcomes().empty());
}
TEST_CASE(permitted_effect_checks_immunity_before_stats_publication) {
    GameplayFixture f;auto& target=f.actor();
    CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    InteractionPolicy policy{InteractionIntent::Beneficial,mask(AllowedTarget::Friendly),91,7,true};
    target.tags={7};target.immuneEffects={91};
    Effect boost{91,91,UINT64_MAX,1,{{.handle=91,.rule=1,.target=statId("power"),.zone=1,.value=5000}}};
    const auto version=f.stats(f.target).version();
    auto denied=f.combat->applyEffect(f.source,f.target,boost,EffectPolicy::Independent,policy,true).value();
    CHECK_TRUE(denied.decision.denial==InteractionDenial::Immune && denied.effectInstance==0);
    CHECK_TRUE(f.stats(f.target).version()==version && f.stats(f.target).read(statId("power"))->effective==20);
    target.immuneEffects.clear();
    auto applied=f.combat->applyEffect(f.source,f.target,boost,EffectPolicy::Independent,policy,true).value();
    CHECK_TRUE(applied.decision.allowed && applied.effectInstance==91 && applied.source.team==10);
    CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==30);
    CHECK_TRUE(f.stats(f.target).removeEffect(91).has_value());
    CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==20);
    policy.effectType=0;
    CHECK_TRUE(f.combat->applyEffect(f.source,f.target,boost,EffectPolicy::Independent,policy,true).error().code==ayt::combat::Error::Invalid);
}
TEST_CASE(permitted_capacity_effect_updates_life_without_business_sync) {
    GameplayFixture f;InteractionPolicy harmful{InteractionIntent::Harmful,mask(AllowedTarget::Hostile),55};
    Effect suppress{55,55,UINT64_MAX,1,{{.handle=55,.rule=1,.target=statId("capacity"),.zone=1,.value=-100}}};
    auto applied=f.combat->applyEffect(f.source,f.target,suppress,EffectPolicy::Independent,harmful).value();
    CHECK_TRUE(applied.decision.allowed && applied.effectInstance==55);
    CHECK_TRUE(f.hp()==0 && f.actor().life()->phase()==LifePhase::Dead);
    CHECK_TRUE(f.combat->outcomes().empty() && f.combat->hits().activeAttacks()==0);
    CHECK_TRUE(f.stats(f.target).removeEffect(55).has_value());
    CHECK_TRUE(f.hp()==0 && f.actor().life()->phase()==LifePhase::Dead);
}
TEST_CASE(owned_instances_from_two_sources_keep_independent_numeric_and_status_contributions) {
    GameplayFixture f;auto other=f.additionalActor(10);
    CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    InteractionPolicy policy{InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    EffectDefinition definition;definition.type=55;definition.key=77;definition.maxStacks=2;
    definition.modifiers={{.rule=1,.target=statId("power"),.zone=1,.value=5000}};
    definition.tags={7};definition.immuneEffects={91};definition.stunned=true;
    auto first=f.combat->applyOwnedEffect(f.source,f.target,definition,policy).value();
    auto second=f.combat->applyOwnedEffect(other,f.target,definition,policy).value();
    CHECK_TRUE(first.decision.allowed && second.decision.allowed && first.effectInstance!=second.effectInstance);
    CHECK_TRUE(f.combat->ownedEffectCount()==2 && f.stats(f.target).read(statId("power"))->effective==40);
    CHECK_TRUE(f.combat->hasTag(f.target,7).value() && f.combat->immuneToEffect(f.target,91).value() && f.combat->stunned(f.target).value());
    CHECK_TRUE(f.combat->damage(f.target,f.source,10).error().code==ayt::combat::Error::Invalid);
    EffectDefinition denied;denied.type=91;denied.key=91;denied.tags={9};
    CHECK_TRUE(f.combat->applyOwnedEffect(f.source,f.target,denied,policy)->decision.denial==InteractionDenial::Immune);
    auto mismatched=definition;mismatched.maxStacks=3;
    CHECK_TRUE(f.combat->applyOwnedEffect(f.source,f.target,mismatched,policy).error().code==ayt::combat::Error::Invalid);
    const auto version=f.stats(f.target).version();
    CHECK_TRUE(f.combat->applyOwnedEffect(f.source,f.target,definition,policy).error().code==ayt::combat::Error::Limit);
    CHECK_TRUE(f.combat->ownedEffectCount()==2 && f.stats(f.target).version()==version);
    CHECK_TRUE(f.combat->removeOwnedEffect(first.effectInstance).has_value());
    CHECK_TRUE(f.combat->ownedEffectCount()==1 && f.stats(f.target).read(statId("power"))->effective==30);
    CHECK_TRUE(f.combat->hasTag(f.target,7).value() && f.combat->immuneToEffect(f.target,91).value() && f.combat->stunned(f.target).value());
    CHECK_TRUE(f.combat->removeOwnedEffect(second.effectInstance).has_value());
    CHECK_TRUE(f.combat->ownedEffectCount()==0 && f.stats(f.target).read(statId("power"))->effective==20);
    CHECK_TRUE(!f.combat->hasTag(f.target,7).value() && !f.combat->immuneToEffect(f.target,91).value() && !f.combat->stunned(f.target).value());
}
TEST_CASE(owned_effect_replace_refresh_failure_and_expiry_keep_one_instance) {
    GameplayFixture f;CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    InteractionPolicy policy{InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    EffectDefinition first;first.type=55;first.key=77;first.durationTicks=2;first.maxStacks=2;
    first.modifiers={{.rule=1,.target=statId("power"),.zone=1,.value=5000}};first.tags={7};
    const auto id=f.combat->applyOwnedEffect(f.source,f.target,first,policy)->effectInstance;
    CHECK_TRUE(id && f.stats(f.target).read(statId("power"))->effective==30);
    auto invalid=first;invalid.modifiers[0].target=statId("missing");invalid.tags={8};
    const auto version=f.stats(f.target).version();
    CHECK_FALSE(f.combat->replaceOwnedEffect(id,invalid).has_value());
    CHECK_TRUE(f.stats(f.target).version()==version && f.combat->hasTag(f.target,7).value() && !f.combat->hasTag(f.target,8).value());
    auto replacement=first;replacement.tags={8};replacement.immuneEffects={91};replacement.stunned=true;
    replacement.modifiers[0].value=10000;
    CHECK_TRUE(f.combat->replaceOwnedEffect(id,replacement).has_value());
    CHECK_TRUE(f.combat->ownedEffectCount()==1 && f.stats(f.target).read(statId("power"))->effective==40);
    CHECK_TRUE(!f.combat->hasTag(f.target,7).value() && f.combat->hasTag(f.target,8).value());
    auto statusOnly=replacement;statusOnly.modifiers.clear();
    CHECK_TRUE(f.combat->replaceOwnedEffect(id,statusOnly).has_value());
    CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==20 && f.combat->hasTag(f.target,8).value());
    CHECK_TRUE(f.combat->replaceOwnedEffect(id,replacement).has_value());
    CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==40 && f.combat->ownedEffectCount()==1);
    CHECK_TRUE(f.combat->refreshOwnedEffect(id,3).has_value());
    f.world.fixedUpdate(.016f);f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->ownedEffectCount()==1 && f.combat->stunned(f.target).value());
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->ownedEffectCount()==0 && f.stats(f.target).read(statId("power"))->effective==20);
    CHECK_TRUE(!f.combat->hasTag(f.target,8).value() && !f.combat->immuneToEffect(f.target,91).value());
}
TEST_CASE(owned_effect_source_cleanup_and_failed_apply_leave_no_contribution) {
    GameplayFixture f;CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    InteractionPolicy policy{InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    EffectDefinition effect;effect.type=55;effect.key=77;effect.tags={7};
    effect.modifiers={{.rule=1,.target=statId("power"),.zone=1,.value=5000}};
    auto bad=effect;bad.modifiers[0].target=statId("missing");
    const auto version=f.stats(f.target).version();
    CHECK_FALSE(f.combat->applyOwnedEffect(f.source,f.target,bad,policy).has_value());
    CHECK_TRUE(f.stats(f.target).version()==version && f.combat->ownedEffectCount()==0 && !f.combat->hasTag(f.target,7).value());
    auto first=f.combat->applyOwnedEffect(f.source,f.target,effect,policy).value();
    CHECK_TRUE(first.effectInstance && f.combat->hasTag(f.target,7).value());
    CHECK_TRUE(f.combat->removeSourceEffects(f.source).value()==1);
    CHECK_TRUE(f.combat->ownedEffectCount()==0 && f.stats(f.target).read(statId("power"))->effective==20);
    auto second=f.combat->applyOwnedEffect(f.source,f.target,effect,policy).value();
    CHECK_TRUE(second.effectInstance!=first.effectInstance);
    f.world.destroyEntity(f.world.getEntityByHandle(f.source));
    CHECK_TRUE(f.combat->hasTag(f.target,7).value() && f.stats(f.target).read(statId("power"))->effective==30);
    CHECK_TRUE(f.combat->refreshOwnedEffect(second.effectInstance,2).error().code==ayt::combat::Error::Stale);
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->ownedEffectCount()==0 && f.stats(f.target).read(statId("power"))->effective==20);
    CHECK_TRUE(!f.combat->hasTag(f.target,7).value());
}
TEST_CASE(status_only_effect_and_component_replacement_release_owned_facts) {
    GameplayFixture f;CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    InteractionPolicy policy{InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    EffectDefinition status;status.type=55;status.key=77;status.durationTicks=1;
    status.tags={7};status.immuneEffects={91};status.stunned=true;
    const auto version=f.stats(f.target).version();
    auto id=f.combat->applyOwnedEffect(f.source,f.target,status,policy)->effectInstance;
    CHECK_TRUE(id && f.stats(f.target).version()==version && f.combat->stunned(f.target).value());
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->ownedEffectCount()==0 && !f.combat->stunned(f.target).value() && f.stats(f.target).version()==version);
    status.durationTicks=UINT64_MAX;
    auto numeric=status;numeric.modifiers={{.rule=1,.target=statId("power"),.zone=1,.value=5000}};
    CHECK_TRUE(f.combat->applyOwnedEffect(f.source,f.target,numeric,policy)->effectInstance!=id);
    CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==30);
    auto* entity=f.world.getEntityByHandle(f.target);
    entity->removeComponent<CombatComponent>();entity->addComponent<CombatComponent>();
    CHECK_TRUE(!f.combat->hasTag(f.target,7).value());
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->ownedEffectCount()==0 && f.stats(f.target).read(statId("power"))->effective==20);
}
TEST_CASE(area_selected_friendly_members_enter_hold_and_exit_without_duplicate_effects) {
    GameplayFixture f;CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    std::vector<AreaCandidate> candidates{{f.target,false}};
    AreaDefinition area;area.effect.type=55;area.effect.key=77;
    area.effect.modifiers={{.rule=1,.target=statId("power"),.zone=1,.value=5000}};
    area.permission={InteractionIntent::Beneficial,mask(AllowedTarget::Friendly),0,0,true};
    auto id=f.combat->startArea(f.source,area,[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{return candidates;}).value();
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==0 && f.stats(f.target).read(statId("power"))->effective==20);
    f.actor().immuneEffects={55};candidates={{f.target,true}};
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==0 && f.combat->lastAreaSweep(id)->targets[0].denial==InteractionDenial::Immune);
    f.actor().immuneEffects.clear();
    candidates={{f.target,false},{f.target,true}};
    f.world.fixedUpdate(.016f);
    const auto debug=f.combat->debugAreas();
    CHECK(debug.size()==1 && debug[0].source==f.source && debug[0].sweep.area==id);
    auto entered=f.combat->lastAreaSweep(id).value();
    CHECK_TRUE(entered.targets.size()==1 && entered.targets[0].state==AreaTargetState::Entered);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1 && f.combat->ownedEffectCount()==1);
    CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==30);
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->lastAreaSweep(id)->targets[0].state==AreaTargetState::Retained);
    CHECK(debug[0].sweep.targets[0].state==AreaTargetState::Entered);
    CHECK_TRUE(f.combat->ownedEffectCount()==1 && f.stats(f.target).read(statId("power"))->effective==30);
    f.world.updatePresentation(.1f,1);candidates={{f.target,false}};
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1);
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==0 && f.stats(f.target).read(statId("power"))->effective==20);
    candidates={{f.target,true}};f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1);
    CHECK_TRUE(f.combat->setRelation(10,20,Relation::Hostile).has_value());f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==0 && f.stats(f.target).read(statId("power"))->effective==20);
    CHECK_TRUE(f.combat->stopArea(id)->ended && f.combat->ownedEffectCount()==0);
}
TEST_CASE(area_dynamic_tag_and_target_death_recheck_current_eligibility) {
    GameplayFixture f;CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    std::vector<AreaCandidate> candidates{{f.target,true}};
    AreaDefinition area;area.effect.type=55;area.effect.key=55;area.effect.tags={8};
    area.permission={InteractionIntent::Beneficial,mask(AllowedTarget::Friendly),0,7,true};
    auto feedback=area;feedback.effect.tags={7};
    CHECK_TRUE(f.combat->startArea(f.source,feedback,[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{return candidates;}).error().code==ayt::combat::Error::Invalid);
    feedback=area;feedback.effect.immuneEffects={55};
    CHECK_TRUE(f.combat->startArea(f.source,feedback,[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{return candidates;}).error().code==ayt::combat::Error::Invalid);
    auto id=f.combat->startArea(f.source,area,[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{return candidates;}).value();
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.combat->areaMemberCount(id).value()==0);
    EffectDefinition qualifying;qualifying.type=44;qualifying.key=44;qualifying.tags={7};
    InteractionPolicy friendly{InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    auto tag=f.combat->applyOwnedEffect(f.source,f.target,qualifying,friendly).value();
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1 && f.combat->hasTag(f.target,8).value());
    CHECK_TRUE(f.combat->removeOwnedEffect(tag.effectInstance).has_value());
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==0 && !f.combat->hasTag(f.target,8).value());
    f.actor().tags={7};f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1);
    CHECK_TRUE(f.combat->attributes().set(f.target,"vitality",0).has_value());
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==0 && !f.combat->hasTag(f.target,8).value());
}
TEST_CASE(area_candidate_limit_and_provider_failure_preserve_previous_members) {
    GameplayFixture f;auto other=f.additionalActor(20);
    CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    std::vector<AreaCandidate> candidates{{f.target,true}};bool throws=false;
    AreaDefinition area;area.effect.type=55;area.effect.key=55;area.effect.tags={8};
    area.permission={InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    area.maxCandidates=1;area.maxMembers=1;
    auto id=f.combat->startArea(f.source,area,[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{
        if(throws)throw std::runtime_error("provider failure");return candidates;}).value();
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.combat->areaMemberCount(id).value()==1);
    candidates={{other,true},{f.target,true}};
    CHECK_TRUE(f.combat->updateArea(id).error().code==ayt::combat::Error::Limit);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1 && f.combat->hasTag(f.target,8).value());
    throws=true;
    CHECK_TRUE(f.combat->updateArea(id).error().code==ayt::combat::Error::Invalid);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1 && f.combat->hasTag(f.target,8).value());
    throws=false;candidates.clear();f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==0 && !f.combat->hasTag(f.target,8).value());
}
TEST_CASE(area_member_limit_and_stable_order_do_not_truncate_previous_members) {
    GameplayFixture f;auto other=f.additionalActor(20);
    CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    std::vector<AreaCandidate> candidates{{f.target,true}};
    AreaDefinition area;area.effect.type=55;area.effect.key=55;area.effect.tags={8};
    area.permission={InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    area.maxMembers=1;
    auto id=f.combat->startArea(f.source,area,[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{return candidates;}).value();
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.combat->areaMemberCount(id).value()==1);
    candidates={{other,true},{f.target,true}};
    CHECK_TRUE(f.combat->updateArea(id).error().code==ayt::combat::Error::Limit);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1 && f.combat->hasTag(f.target,8).value());
    CHECK_TRUE(!f.combat->hasTag(other,8).value());
    candidates={{other,true}};
    auto sweep=f.combat->updateArea(id).value();
    CHECK_TRUE(sweep.targets.size()==2 && sweep.targets[0].state==AreaTargetState::Exited &&
        sweep.targets[1].state==AreaTargetState::Entered);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1 && f.combat->hasTag(other,8).value());
}
TEST_CASE(area_reports_each_target_when_one_numeric_schema_rejects_the_effect) {
    GameplayFixture f;auto other=f.additionalActor(20);
    CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    auto limitedSchema=Schema::compile({{.name="vitality",.shape=Shape::Pool,.current=100,.capacity=100}}).value();
    CHECK_TRUE(f.stats(other).configure(limitedSchema).has_value());
    std::vector<AreaCandidate> candidates{{other,true},{f.target,true}};
    AreaDefinition area;area.effect.type=55;area.effect.key=55;
    area.effect.modifiers={{.rule=1,.target=statId("power"),.zone=1,.value=5000}};
    area.permission={InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    auto id=f.combat->startArea(f.source,area,[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{return candidates;}).value();
    auto sweep=f.combat->updateArea(id).value();
    CHECK_TRUE(sweep.targets.size()==2 && sweep.targets[0].target==f.target &&
        sweep.targets[0].state==AreaTargetState::Entered && sweep.targets[1].target==other &&
        sweep.targets[1].state==AreaTargetState::Failed && sweep.targets[1].error.has_value());
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1 && f.stats(f.target).read(statId("power"))->effective==30);
    CHECK_TRUE(f.stats(other).read(statId("vitality"))->current==100);
    CHECK_TRUE(f.combat->stopArea(id)->ended && f.stats(f.target).read(statId("power"))->effective==20);
}
TEST_CASE(area_source_death_or_cancellation_removes_only_its_members) {
    GameplayFixture f;CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    int ended=0;f.combat->setOnAreaSweep([&](const AreaSweep& sweep){if(sweep.ended)++ended;});
    std::vector<AreaCandidate> candidates{{f.target,true}};
    AreaDefinition area;area.effect.type=55;area.effect.key=55;
    area.effect.modifiers={{.rule=1,.target=statId("power"),.zone=1,.value=5000}};
    area.permission={InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    auto provider=[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{return candidates;};
    auto id=f.combat->startArea(f.source,area,provider).value();
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==30);
    CHECK_TRUE(f.combat->stopArea(id)->ended && f.stats(f.target).read(statId("power"))->effective==20);
    id=f.combat->startArea(f.source,area,provider).value();
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.combat->removeSourceEffects(f.source).value()==1);
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).error().code==ayt::combat::Error::Missing &&
        f.stats(f.target).read(statId("power"))->effective==20);
    id=f.combat->startArea(f.source,area,provider).value();
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.combat->areaMemberCount(id).value()==1);
    CHECK_TRUE(f.combat->attributes().set(f.source,"vitality",0).has_value());
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).error().code==ayt::combat::Error::Missing);
    CHECK_TRUE(ended==1 && f.combat->ownedEffectCount()==0 && f.stats(f.target).read(statId("power"))->effective==20);
}
TEST_CASE(area_source_destruction_and_target_replacement_clean_members) {
    GameplayFixture f;CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    std::vector<AreaCandidate> candidates{{f.target,true}};
    AreaDefinition area;area.effect.type=55;area.effect.key=55;area.effect.tags={8};
    area.permission={InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    auto provider=[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{return candidates;};
    auto id=f.combat->startArea(f.source,area,provider).value();
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.combat->areaMemberCount(id).value()==1);
    auto* target=f.world.getEntityByHandle(f.target);
    target->removeComponent<CombatComponent>();target->addComponent<CombatComponent>();
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==1 && f.combat->hasTag(f.target,8).value());
    f.world.destroyEntity(f.world.getEntityByHandle(f.source));f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).error().code==ayt::combat::Error::Missing);
    CHECK_TRUE(!f.combat->hasTag(f.target,8).value() && f.combat->ownedEffectCount()==0);
}
TEST_CASE(area_target_destruction_does_not_keep_a_stale_member) {
    GameplayFixture f;CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    std::vector<AreaCandidate> candidates{{f.target,true}};
    AreaDefinition area;area.effect.type=55;area.effect.key=55;area.effect.tags={8};
    area.permission={InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    auto id=f.combat->startArea(f.source,area,[&]()->ayt::combat::Result<std::vector<AreaCandidate>>{return candidates;}).value();
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.combat->areaMemberCount(id).value()==1);
    f.world.destroyEntity(f.world.getEntityByHandle(f.target));
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(f.combat->areaMemberCount(id).value()==0 && f.combat->ownedEffectCount()==0);
    CHECK_TRUE(f.combat->lastAreaSweep(id)->targets.back().state==AreaTargetState::Failed);
}
TEST_CASE(invalid_recipe_shape_input_and_negative_output_never_mutate) {
    GameplayFixture f;const auto version=f.stats(f.target).version();
    auto wrong=GameplayFixture::scaled("mana_only",statId("mana"),AttributeOp::Decrease,10000);
    auto negative=GameplayFixture::scaled("negative",statId("vitality"),AttributeOp::Decrease,-10000);
    Recipe missing{"missing",statId("vitality"),AttributeOp::Decrease,
        CalculationPlan::compile({{.id=1,.op=Op::Input,.binding=0}},1,1).value(),{{InputSource::SourceStat,statId("undefined")}}};
    CHECK_TRUE(f.combat->attributes().setRecipes(RecipeBook::compile({wrong,negative,missing}).value()).has_value());
    CHECK_FALSE(f.combat->damage(f.source,f.target,20,"absent").has_value());
    CHECK_FALSE(f.combat->damage(f.source,f.target,20,"mana_only").has_value());
    CHECK_FALSE(f.combat->damage(f.source,f.target,20,"negative").has_value());
    CHECK_FALSE(f.combat->damage(f.source,f.target,20,"missing").has_value());
    CHECK_FALSE(f.combat->attributes().decrease(f.target,"undefined",20).has_value());
    CHECK_FALSE(f.combat->attributes().decrease(f.target,"mana",-1).has_value());
    CHECK_TRUE(f.hp()==100 && f.stats(f.target).version()==version && f.combat->hits().activeAttacks()==0);
}
TEST_CASE(direct_stats_callbacks_see_synchronized_life_without_damage_attribution) {
    GameplayFixture f;int callbacks=0,damageEvents=0,lifeEvents=0;
    f.combat->setOnOutcome([&](const auto&){++damageEvents;});f.combat->setOnLifeChanged([&](const auto&){++lifeEvents;});
    f.stats(f.target).setOnSettled([&](const auto&){++callbacks;CHECK_TRUE(f.actor().life()->phase()==LifePhase::Dead);throw std::runtime_error("business callback");});
    CHECK_TRUE(f.combat->attributes().decrease(f.target,"vitality",100).has_value());
    CHECK_TRUE(callbacks==1 && f.actor().life()->phase()==LifePhase::Dead && f.stats(f.target).lastError().has_value());
    f.stats(f.target).setOnSettled({});
    CHECK_TRUE(f.combat->attributes().increase(f.target,"vitality",10).has_value());
    CHECK_TRUE(f.actor().life()->phase()==LifePhase::Dead); // Default policy does not revive.
    f.world.fixedUpdate(.016f);CHECK_TRUE(lifeEvents==1 && damageEvents==0);
}
TEST_CASE(capacity_base_fields_and_effect_expiry_observe_final_pool_current) {
    GameplayFixture f(true);
    CHECK_TRUE(f.combat->attributes().decrease(f.target,"capacity",50).has_value());
    CHECK_TRUE(f.hp()==50 && f.actor().life()->phase()==LifePhase::Alive);
    Effect suppress{11,11,2,1,{{.handle=11,.rule=1,.target=statId("capacity"),.zone=1,.value=-50}}};
    CHECK_TRUE(f.stats(f.target).applyEffect(suppress,EffectPolicy::Independent,0).has_value());
    CHECK_TRUE(f.hp()==0 && f.actor().life()->phase()==LifePhase::Dead);
    CHECK_TRUE(f.stats(f.target).removeEffect(11).has_value());CHECK_TRUE(f.hp()==0);
    CHECK_TRUE(f.stats(f.target).configure(f.stats(f.target).state()->schema()).has_value());
    CHECK_TRUE(f.actor().life()->phase()==LifePhase::Alive);
    CHECK_TRUE(f.combat->attributes().set(f.target,"capacity",0).has_value());
    Effect temporary{12,12,1,1,{{.handle=12,.rule=1,.target=statId("capacity"),.zone=1,.value=100}}};
    // A temporary positive capacity on a zero base can expire and clamp current to zero.
    CHECK_TRUE(f.stats(f.target).applyEffect(temporary,EffectPolicy::Independent,0).has_value());
    CHECK_TRUE(f.combat->attributes().set(f.target,"vitality",100).has_value());
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.hp()==0 && f.actor().life()->phase()==LifePhase::Dead);
}
TEST_CASE(atomic_intermediate_zero_does_not_emit_death_and_revival_is_explicit) {
    GameplayFixture f(true);
    std::array mutations{Mutation{statId("vitality"),MutationOp::Decrease,100},Mutation{statId("vitality"),MutationOp::Increase,20}};
    CHECK_TRUE(f.stats(f.target).commit(mutations,f.stats(f.target).version()).has_value());
    CHECK_TRUE(f.hp()==20 && f.actor().life()->phase()==LifePhase::Alive && f.actor().life()->sequence()==0);
    CHECK_TRUE(f.combat->damage(f.source,f.target,30)->life->after==LifePhase::Dead);
    CHECK_TRUE(f.combat->heal(f.target,f.target,10)->life->after==LifePhase::Alive);
    CHECK_TRUE(f.combat->damage(f.source,f.target,30)->life->sequence==3);
}
TEST_CASE(reconfiguration_restore_and_component_replacement_reconnect_automatically) {
    GameplayFixture f;const auto old=bindCombatant(f.world,f.target).value();
    const auto saved=f.stats(f.target).checkpoint().value();
    CHECK_TRUE(f.combat->damage(f.source,f.target,100).has_value());
    CHECK_TRUE(f.stats(f.target).restoreCheckpoint(saved).has_value());
    CHECK_TRUE(f.actor().life()->phase()==LifePhase::Alive && bindCombatant(f.world,f.target).has_value());
    auto* entity=f.world.getEntityByHandle(f.target);auto schema=f.stats(f.target).state()->schema();
    entity->removeComponent<StatsComponent>();auto* replacement=entity->addComponent<StatsComponent>();
    CHECK_TRUE(replacement->configure(schema).has_value());CHECK_TRUE(f.combat->damage(f.source,f.target,10).has_value());
    entity->removeComponent<CombatComponent>();entity->addComponent<CombatComponent>();
    CHECK_TRUE(f.actor().team==20 && f.actor().life()->phase()==LifePhase::Alive);
    CHECK_TRUE(f.combat->damage(f.source,f.target,10).has_value() && f.hp()==80);
    ayt::combat::SettlementRequest late;late.hit.target=old.ref;
    CHECK_TRUE(f.combat->submit(old,late,GameplayFixture::scaled("x",statId("vitality"),AttributeOp::Decrease,10000).plan,{}).error().code==ayt::combat::Error::Stale);
}
TEST_CASE(attach_existing_stats_preserves_values_effects_and_generation) {
    GameplayFixture f;
    auto* entity=f.world.createEntity();auto handle=f.world.getEntityHandle(entity->getId());
    auto* stats=entity->addComponent<StatsComponent>();
    auto schema=Schema::compile({
        {.name="vitality",.shape=Shape::Pool,.current=100,.capacity=100},
        {.name="armor",.mode=Mode::Advanced,.initial=20,.maximum=1000,.zones={{1,ZoneOp::Flat}}}
    }).value();
    CHECK_TRUE(stats->configure(schema).has_value());
    const Mutation wounded{statId("vitality"),MutationOp::Decrease,30};
    CHECK_TRUE(stats->commit(std::span(&wounded,1),stats->version()).has_value());
    Effect boost{61,61,UINT64_MAX,1,{{.handle=61,.rule=1,.target=statId("armor"),.zone=1,.value=15}}};
    CHECK_TRUE(stats->applyEffect(boost,EffectPolicy::Independent,0).has_value());
    const auto generation=stats->generation(),version=stats->version();
    const auto checkpoint=stats->checkpoint().value();
    CHECK_TRUE(stats->enqueue({Mutation{statId("vitality"),MutationOp::Decrease,5}}).has_value());
    CHECK_FALSE(f.combat->attachActor(handle,{statId("unknown")},30).has_value());
    CHECK_TRUE(entity->getComponent<CombatComponent>()==nullptr);
    stats->setAuthority(false);
    CHECK_FALSE(f.combat->attachActor(handle,{statId("vitality")},30).has_value());
    CHECK_TRUE(entity->getComponent<CombatComponent>()==nullptr);
    stats->setAuthority(true);
    CHECK_TRUE(f.combat->attachActor(handle,{statId("vitality")},30).has_value());
    CHECK_TRUE(entity->getComponent<StatsComponent>()==stats && stats->generation()==generation && stats->version()==version);
    CHECK_TRUE(stats->checkpoint().value()==checkpoint);
    CHECK_TRUE(stats->read(statId("vitality"))->current==70 && stats->read(statId("armor"))->effective==35);
    auto* actor=entity->getComponent<CombatComponent>();
    CHECK_TRUE(actor && actor->team==30 && actor->life() && actor->life()->phase()==LifePhase::Alive);
    f.world.fixedUpdate(.016f);
    CHECK_TRUE(stats->read(statId("vitality"))->current==65 && actor->life()->phase()==LifePhase::Alive);
    CHECK_TRUE(f.combat->setRelation(10,30,Relation::Hostile).has_value());
    auto hit=f.combat->damage(f.source,handle,20).value();
    CHECK_TRUE(hit.actual==20 && hit.remaining==45 && stats->read(statId("armor"))->effective==35);
    entity->removeComponent<CombatComponent>();
    actor=entity->addComponent<CombatComponent>();
    CHECK_TRUE(actor->team==30 && actor->life() && actor->life()->phase()==LifePhase::Alive);
    CHECK_TRUE(stats->read(statId("vitality"))->current==45 && stats->read(statId("armor"))->effective==35);
}
TEST_CASE(batch_publication_completes_life_before_any_bank_callback) {
    GameplayFixture f;auto& a=f.stats(f.source);auto& b=f.stats(f.target);
    int callbacks=0;
    a.setOnSettled([&](const auto&){++callbacks;CHECK_TRUE(f.actor().life()->phase()==LifePhase::Dead);});
    b.setOnSettled([&](const auto&){++callbacks;CHECK_TRUE(f.world.getEntityByHandle(f.source)->getComponent<CombatComponent>()->life()->phase()==LifePhase::Dead);});
    std::array handles{f.source,f.target};std::array generations{a.generation(),b.generation()};std::array versions{a.version(),b.version()};
    auto plan=std::make_shared<const CalculationPlan>(CalculationPlan::compile({{.id=1,.op=Op::Input,.binding=0}},1,1).value());
    // Stats bank request fields are deliberately explicit here; gameplay callers do not build them.
    std::array<ayt::stats::SettlementRequest,2> bank{};
    for(std::uint32_t i=0;i<2;++i){bank[i].id=i+1;bank[i].plan=plan;bank[i].inputs={{.literal=100}};bank[i].context={.calculation=i+1,.hit=i+1};bank[i].writes={{i,statId("vitality"),MutationOp::Decrease}};}
    CHECK_TRUE(settleEntityBatch(f.world,f.combat->worldToken(),handles,generations,versions,bank,BatchSemantics::Ordered).has_value());
    CHECK_TRUE(callbacks==2 && f.hp()==0);
}
TEST_CASE(notification_failures_and_simple_reentry_cannot_reapply_damage) {
    GameplayFixture f;int called=0;
    f.combat->setOnOutcome([&](const auto&){++called;CHECK_TRUE(f.combat->damage(f.source,f.target,10).error().code==ayt::combat::Error::Reentrant);throw std::runtime_error("consumer failure");});
    CHECK_TRUE(f.combat->damage(f.source,f.target,10).has_value());f.world.fixedUpdate(.016f);
    CHECK_TRUE(called==1 && f.hp()==90 && f.combat->lastError().has_value() && f.combat->outcomes().empty());
    f.world.fixedUpdate(.016f);CHECK_TRUE(called==1 && f.hp()==90);
}
TEST_CASE(rejected_targets_replicas_and_invalid_profiles_do_not_modify_state) {
    GameplayFixture f;const auto before=f.stats(f.target).version();
    CHECK_TRUE(f.combat->setRelation(10,20,Relation::Friendly).has_value());
    CHECK_TRUE(f.combat->damage(f.source,f.target,10)->kind==OutcomeKind::Rejected && f.hp()==100);
    f.actor().invulnerable=true;CHECK_TRUE(f.combat->setRelation(10,20,Relation::Hostile).has_value());
    CHECK_TRUE(f.combat->damage(f.source,f.target,10)->kind==OutcomeKind::Rejected);
    f.stats(f.target).setAuthority(false);CHECK_FALSE(f.combat->damage(f.source,f.target,10).has_value());
    f.stats(f.target).setAuthority(true);
    ActorProfile invalid{f.stats(f.target).state()->schema(),{statId("power")},20};
    CHECK_FALSE(f.combat->configureActor(f.target,invalid).has_value());
    CHECK_TRUE(f.stats(f.target).version()==before && f.hp()==100);
}
TEST_CASE(effect_update_retains_other_sources_and_failed_update_keeps_state) {
    GameplayFixture f;
    Effect first{21,7,UINT64_MAX,1,{{.handle=21,.rule=1,.target=statId("power"),.zone=1,.value=5000}}};
    Effect second{22,7,UINT64_MAX,1,{{.handle=22,.rule=1,.target=statId("power"),.zone=1,.value=2500}}};
    CHECK_TRUE(f.stats(f.target).applyEffect(first,EffectPolicy::Independent,0).has_value());
    CHECK_TRUE(f.stats(f.target).applyEffect(second,EffectPolicy::Independent,0).has_value());
    first.modifiers[0].value=10000;
    CHECK_TRUE(f.stats(f.target).updateEffect(first,0).has_value());CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==45);
    const auto version=f.stats(f.target).version();first.modifiers[0].target=statId("absent");
    CHECK_FALSE(f.stats(f.target).updateEffect(first,0).has_value());CHECK_TRUE(f.stats(f.target).version()==version);
    CHECK_TRUE(f.stats(f.target).removeEffect(21).has_value());CHECK_TRUE(f.stats(f.target).read(statId("power"))->effective==25);
}
TEST_CASE(capacity_effect_update_has_no_transient_removal_clamp_or_false_death) {
    GameplayFixture f(true);
    CHECK_TRUE(f.combat->attributes().set(f.target,"capacity",0).has_value());
    Effect capacity{31,31,UINT64_MAX,1,{{.handle=31,.rule=1,.target=statId("capacity"),.zone=1,.value=100}}};
    CHECK_TRUE(f.stats(f.target).applyEffect(capacity,EffectPolicy::Independent,0).has_value());
    CHECK_TRUE(f.combat->attributes().set(f.target,"vitality",100).has_value());
    const auto sequence=f.actor().life()->sequence();
    CHECK_TRUE(f.stats(f.target).updateEffect(capacity,0).has_value());
    CHECK_TRUE(f.hp()==100 && f.actor().life()->phase()==LifePhase::Alive && f.actor().life()->sequence()==sequence);
    capacity.modifiers[0].value=50;CHECK_TRUE(f.stats(f.target).updateEffect(capacity,0).has_value());
    CHECK_TRUE(f.hp()==50 && f.actor().life()->phase()==LifePhase::Alive);
}
TEST_CASE(removing_one_effect_rebuilds_final_capacity_once) {
    GameplayFixture f;
    Effect balanced{41,41,UINT64_MAX,1,{
        {.handle=41,.rule=1,.target=statId("capacity"),.zone=1,.value=100},
        {.handle=42,.rule=2,.target=statId("capacity"),.zone=1,.value=-100}}};
    CHECK_TRUE(f.stats(f.target).applyEffect(balanced,EffectPolicy::Independent,0).has_value());
    CHECK_TRUE(f.hp()==100);CHECK_TRUE(f.stats(f.target).removeEffect(41).has_value());
    CHECK_TRUE(f.hp()==100 && f.actor().life()->phase()==LifePhase::Alive);
}
TEST_CASE(expiring_balanced_contributions_at_one_tick_does_not_cause_false_death) {
    GameplayFixture f;
    Effect first{51,51,1,1,{{.handle=51,.rule=1,.target=statId("capacity"),.zone=1,.value=100}}};
    Effect second{52,52,1,1,{{.handle=52,.rule=1,.target=statId("capacity"),.zone=1,.value=-100}}};
    CHECK_TRUE(f.stats(f.target).applyEffect(first,EffectPolicy::Independent,0).has_value());
    CHECK_TRUE(f.stats(f.target).applyEffect(second,EffectPolicy::Independent,0).has_value());
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.hp()==100 && f.actor().life()->phase()==LifePhase::Alive && f.actor().life()->sequence()==0);
}
TEST_CASE(life_notification_backpressure_rejects_before_direct_and_batch_writes) {
    GameplayFixture f(true);
    for(int i=0;i<1023;++i)if(!f.combat->attributes().set(f.target,"vitality",i%2?1:0))throw std::runtime_error("life queue filled early");
    auto& a=f.stats(f.source);auto& b=f.stats(f.target);
    std::array handles{f.source,f.target};std::array generations{a.generation(),b.generation()};std::array versions{a.version(),b.version()};
    CHECK_TRUE(settleEntityBatch(f.world,f.combat->worldToken(),handles,generations,versions,{},BatchSemantics::Ordered).error().code==ayt::stats::Error::Limit);
    CHECK_TRUE(a.version()==versions[0] && b.version()==versions[1]);
    CHECK_TRUE(f.combat->attributes().set(f.target,"vitality",1).has_value());
    const auto version=b.version();CHECK_TRUE(f.combat->attributes().set(f.target,"vitality",0).error().code==ayt::stats::Error::Limit);
    CHECK_TRUE(b.version()==version && f.hp()==1);
    f.world.fixedUpdate(.016f);CHECK_TRUE(f.combat->attributes().set(f.target,"vitality",0).has_value());
}
TEST_CASE(presentation_does_not_advance_notifications_and_default_recipe_overflow_is_atomic) {
    GameplayFixture f;
    int events=0;f.combat->setOnOutcome([&](const auto&){++events;});
    auto overflowing=GameplayFixture::scaled("overflow",statId("vitality"),AttributeOp::Decrease,20000,true);
    CHECK_TRUE(f.combat->attributes().setRecipes(RecipeBook::compile({overflowing}).value()).has_value());
    const auto version=f.stats(f.target).version();
    CHECK_FALSE(f.combat->damage(f.source,f.target,INT64_MAX).has_value());
    CHECK_TRUE(f.hp()==100 && f.stats(f.target).version()==version);
    CHECK_TRUE(f.combat->damage(f.source,f.target,10).has_value());
    f.world.updatePresentation(.1f,1);CHECK_TRUE(events==0);
    f.world.fixedUpdate(.016f);CHECK_TRUE(events==1);
}
TEST_SUITE_END
