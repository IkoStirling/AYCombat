#pragma once
#include <AYCombat/StatsIntegration.h>
#include <AYStats/EntityIntegration.h>

namespace ayt::combat {
/// Entity identity plus both component incarnations. A replacement invalidates queued work.
struct EntityBinding {
    CombatantRef ref;
    std::uint64_t statsGeneration = 0, combatGeneration = 0;
};
#define AY_CURRENT_CLASS CombatComponent
class CombatComponent final : public ayt::entity::IComponent {
public:
    CombatComponent();
    const char* getName() const override { return "Combat"; }
    AY_PROPERTY(std::uint64_t, team, ayt::entity::kAttrSerialize)
    AY_PROPERTY(std::uint32_t, capabilities, ayt::entity::kAttrSerialize)
    AY_PROPERTY(bool, invulnerable, ayt::entity::kAttrSerialize)
    AY_PROPERTY(bool, blocking, ayt::entity::kAttrSerialize)
    std::vector<std::uint64_t> immuneTypes;
    std::uint64_t generation() const noexcept { return _generation; }
    /// Explicitly bind life policy to the current Stats incarnation; initially zero is depleted.
    Result<void> configureLife(const ayt::stats::StatsComponent& stats, LifePolicy policy);
    LifeTracker* life() noexcept { return _life ? &*_life : nullptr; }
    const LifeTracker* life() const noexcept { return _life ? &*_life : nullptr; }
    bool lifeMatches(std::uint64_t statsGeneration) const noexcept { return !_life || _lifeStatsGeneration == statsGeneration; }
private:
    std::uint64_t _generation, _lifeStatsGeneration = 0;
    std::optional<LifeTracker> _life;
};
#undef AY_CURRENT_CLASS
/// World-owned synchronous owner. Consumers explicitly drain committed outcomes after submit.
/// No user callback runs inside settlement; Ability/Combo can consume the owned result values.
class CombatSystem final : public ayt::entity::ISystem {
public:
    CombatSystem();
    const char* getName() const override { return "CombatSystem"; }
    void onUpdate(float) override {} // Submission/draining are explicitly driven by the simulation owner.
    void bind(ayt::entity::World& world, std::uint64_t token) noexcept;
    std::uint64_t worldToken() const noexcept { return _token; }
    RelationTable& relations() noexcept { return _relations; }
    HitLedger& hits() noexcept { return _hits; }
    Result<void> setRules(RuleRegistry rules);
    /// Recommended Entity entry: validates current binding/authority, applies current rules,
    /// commits through Stats with next-tick observers, then retains owned outcomes for consumers.
    Result<CombatOutcome> submit(EntityBinding binding, SettlementRequest request,
        const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs);
    std::span<const CombatOutcome> outcomes() const noexcept { return _outcomes; }
    /// Copy/move consumer work starts after authoritative commit. Draining never invokes callbacks.
    std::vector<CombatOutcome> takeOutcomes();
private:
    ayt::entity::World* _world = nullptr;
    std::uint64_t _token = 0;
    bool _settling = false;
    RelationTable _relations;
    HitLedger _hits;
    RuleRegistry _rules;
    std::vector<CombatOutcome> _outcomes;
    static constexpr std::size_t OutcomeLimit = 1024;
};
/// Capture a current handle plus World token and component generations; stores no pointers.
Result<EntityBinding> bindCombatant(ayt::entity::World& world, ayt::entity::EntityHandle handle);
/// Register types before sealing the registry, then install after registerStatsSystem(world).
Result<void> registerCombatComponent(ayt::entity::ComponentRegistry& registry);
Result<void> registerCombatSystem(ayt::entity::World& world);
} // namespace ayt::combat
