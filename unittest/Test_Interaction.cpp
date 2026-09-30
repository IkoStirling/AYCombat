#include <AYTest.h>
#include <AYCombat/Interaction.h>
#include "Fixtures.h"
#include <array>
using namespace ayt::combat;
TEST_SUITE(CombatInteractionTests)
TEST_CASE(intent_and_relation_are_explicit_not_derived_from_value_sign) {
    auto table=relations();auto s=source();auto t=target();
    InteractionPolicy beneficial{InteractionIntent::Beneficial,mask(AllowedTarget::Friendly)};
    CHECK_TRUE(checkInteraction(s,t,table,beneficial,{})->denial==InteractionDenial::RelationForbidden);
    CHECK_TRUE(table.set(10,20,Relation::Friendly).has_value());
    CHECK_TRUE(checkInteraction(s,t,table,beneficial,{})->allowed);
    beneficial.relationSnapshot=Relation::Hostile;
    CHECK_TRUE(checkInteraction(s,t,table,beneficial,{})->denial==InteractionDenial::RelationForbidden);
    beneficial.relationSnapshot.reset();
    beneficial.intent=InteractionIntent::Harmful;
    CHECK_TRUE(checkInteraction(s,t,table,beneficial,{})->allowed);
    t.capabilities=mask(Capability::Healable);
    CHECK_TRUE(checkInteraction(s,t,table,beneficial,{})->denial==InteractionDenial::MissingCapability);
    beneficial.intent=InteractionIntent::Neutral;
    CHECK_TRUE(checkInteraction(s,t,table,beneficial,{})->allowed);
}
TEST_CASE(selection_tags_and_effect_immunity_are_independent_current_facts) {
    auto table=relations();auto s=source();auto t=target();
    InteractionPolicy policy{InteractionIntent::Beneficial,mask(AllowedTarget::Hostile),9,7,true};
    const std::array<std::uint64_t,1> tags{7},immune{9};
    CHECK_TRUE(checkInteraction(s,t,table,policy,{})->denial==InteractionDenial::NotSelected);
    CHECK_TRUE(checkInteraction(s,t,table,policy,{true})->denial==InteractionDenial::MissingTag);
    CHECK_TRUE(checkInteraction(s,t,table,policy,{true,tags,immune})->denial==InteractionDenial::Immune);
    CHECK_TRUE(checkInteraction(s,t,table,policy,{true,tags,{}})->allowed);
    CHECK_TRUE(checkInteraction(s,t,table,policy,{true,tags,{}})->relation==Relation::Hostile);
}
TEST_CASE(self_environment_and_invalid_policy_have_distinct_outcomes) {
    auto table=relations();auto s=source();auto t=target();
    InteractionPolicy policy{InteractionIntent::Neutral,mask(AllowedTarget::Self)};
    t.ref=*s.instigator;
    CHECK_TRUE(checkInteraction(s,t,table,policy,{})->allowed);
    policy.allowed=mask(AllowedTarget::Environment);
    CHECK_TRUE(checkInteraction(s,t,table,policy,{})->denial==InteractionDenial::SelfForbidden);
    s.kind=SourceKind::Environment;s.instigator.reset();
    CHECK_TRUE(checkInteraction(s,t,table,policy,{})->allowed);
    policy.allowed=0;CHECK_TRUE(checkInteraction(s,t,table,policy,{}).error().code==Error::Invalid);
    policy.allowed=mask(AllowedTarget::Environment);
    t.ref.domain=2;CHECK_TRUE(checkInteraction(s,t,table,policy,{}).error().code==Error::Stale);
    t.ref.domain=1;policy.relationSnapshot=static_cast<Relation>(99);
    CHECK_TRUE(checkInteraction(s,t,table,policy,{}).error().code==Error::Invalid);
}
TEST_SUITE_END
