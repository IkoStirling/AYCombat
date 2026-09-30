#include <AYCombat/Interaction.h>
#include <algorithm>

namespace ayt::combat {
namespace {
auto fail(Error code,const char* message){return std::unexpected(Diagnostic{code,message,{}});}
bool contains(std::span<const std::uint64_t> values,std::uint64_t value) {
    return std::find(values.begin(),values.end(),value)!=values.end();
}
}
Result<InteractionDecision> checkInteraction(const SourceContext& source,const CombatantView& target,
    const RelationTable& relations,InteractionPolicy policy,InteractionFacts facts) {
    if(auto valid=validateSource(source);!valid)return std::unexpected(valid.error());
    if(!target.ref.valid() || (target.capabilities&~std::uint32_t{3}) || !policy.allowed ||
        (policy.allowed&~std::uint32_t{63}) ||
        (policy.relationSnapshot && *policy.relationSnapshot!=Relation::Unknown &&
         *policy.relationSnapshot!=Relation::Friendly && *policy.relationSnapshot!=Relation::Neutral &&
         *policy.relationSnapshot!=Relation::Hostile) ||
        (policy.intent!=InteractionIntent::Beneficial && policy.intent!=InteractionIntent::Harmful && policy.intent!=InteractionIntent::Neutral))
        return fail(Error::Invalid,"invalid target or interaction policy");
    if((source.instigator && source.instigator->domain!=target.ref.domain) ||
        (source.directSource && source.directSource->domain!=target.ref.domain))
        return fail(Error::Stale,"interaction source and target belong to different domains");
    if(!target.active)return InteractionDecision{false,InteractionDenial::Inactive};
    if(policy.intent==InteractionIntent::Harmful && !(target.capabilities&mask(Capability::Damageable)))
        return InteractionDecision{false,InteractionDenial::MissingCapability};
    if(policy.intent==InteractionIntent::Beneficial && !(target.capabilities&mask(Capability::Healable)))
        return InteractionDecision{false,InteractionDenial::MissingCapability};
    if(policy.requireSelected && !facts.selected)return InteractionDecision{false,InteractionDenial::NotSelected};
    if(policy.requiredTag && !contains(facts.tags,policy.requiredTag))
        return InteractionDecision{false,InteractionDenial::MissingTag};
    if(policy.effectType && contains(facts.immuneEffects,policy.effectType))
        return InteractionDecision{false,InteractionDenial::Immune};
    if(source.kind==SourceKind::Environment) {
        const bool allowed=(policy.allowed&mask(AllowedTarget::Environment))!=0;
        return InteractionDecision{allowed,allowed?InteractionDenial::None:InteractionDenial::EnvironmentForbidden};
    }
    if(*source.instigator==target.ref) {
        const bool allowed=(policy.allowed&mask(AllowedTarget::Self))!=0;
        return InteractionDecision{allowed,allowed?InteractionDenial::None:InteractionDenial::SelfForbidden,Relation::Friendly};
    }
    const auto relation=policy.relationSnapshot.value_or(relations.query(source.team,target.team));
    auto category=AllowedTarget::Unknown;
    if(relation==Relation::Friendly)category=AllowedTarget::Friendly;
    else if(relation==Relation::Neutral)category=AllowedTarget::Neutral;
    else if(relation==Relation::Hostile)category=AllowedTarget::Hostile;
    const bool allowed=(policy.allowed&mask(category))!=0;
    return InteractionDecision{allowed,allowed?InteractionDenial::None:
        (relation==Relation::Unknown?InteractionDenial::UnknownRelation:InteractionDenial::RelationForbidden),relation};
}
} // namespace ayt::combat
