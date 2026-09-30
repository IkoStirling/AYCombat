#pragma once
#include <AYCombat/Combat.h>

namespace ayt::combat {
/// Authored meaning of an interaction; it is never inferred from a signed value.
enum class InteractionIntent { Beneficial, Harmful, Neutral };
/// One single-target permission policy. The caller selects candidates before this check.
struct InteractionPolicy {
    InteractionIntent intent=InteractionIntent::Neutral;
    std::uint32_t allowed=0;
    std::uint64_t effectType=0;
    std::uint64_t requiredTag=0;
    bool requireSelected=false;
    /// When present, use the captured relationship instead of the current table.
    std::optional<Relation> relationSnapshot;
};
/// Current target facts. Selection is a per-request candidate fact, not a status timer.
struct InteractionFacts {
    bool selected=false;
    std::span<const std::uint64_t> tags;
    std::span<const std::uint64_t> immuneEffects;
};
enum class InteractionDenial {
    None, Inactive, MissingCapability, NotSelected, MissingTag, Immune,
    SelfForbidden, UnknownRelation, RelationForbidden, EnvironmentForbidden
};
struct InteractionDecision {
    bool allowed=false;
    InteractionDenial denial=InteractionDenial::None;
    Relation relation=Relation::Unknown;
};
/// Pure current-facts check. Invalid policy/identity is an error; ordinary denial is a value.
Result<InteractionDecision> checkInteraction(const SourceContext&,const CombatantView&,
    const RelationTable&,InteractionPolicy,InteractionFacts);
} // namespace ayt::combat
