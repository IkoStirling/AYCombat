#pragma once
#include <AYCombat/StatsIntegration.h>
#include <AYStats/EntityIntegration.h>

namespace ayt::combat {
class StatsPublicationBridge;
/// One-time actor configuration; Stats remains the sole numerical owner.
struct ActorProfile {
    std::shared_ptr<const ayt::stats::Schema> schema;
    LifePolicy life;
    std::uint64_t team=0;
    std::uint32_t capabilities=mask(Capability::Damageable)|mask(Capability::Healable);
};
struct LifeEvent {
    ayt::entity::EntityHandle target;
    LifeTransition transition;
    std::uint64_t statsGeneration=0,combatGeneration=0;
};
/// Simple gameplay result. Actual damage/healing is non-negative; no Stats/ledger bookkeeping.
struct ActionResult {
    OutcomeKind kind=OutcomeKind::Rejected;
    ayt::stats::Value calculated=0,actual=0,remaining=0,excess=0;
    std::uint64_t actionId=0;
    TargetDecision decision;
    RuleDecision rule;
    std::optional<LifeTransition> life;
};
/// Generic attribute result: signed actual delta, before/after values and boundary excess.
struct AttributeResult { ayt::stats::Value before=0,after=0,actual=0,excess=0; };
/// Entity identity plus both component incarnations. A replacement invalidates queued work.
struct EntityBinding {
    CombatantRef ref;
    std::uint64_t statsGeneration = 0, combatGeneration = 0;
};
#define AY_CURRENT_CLASS CombatComponent
class CombatComponent final : public ayt::entity::IComponent {
public:
    CombatComponent();
    void onAttach(ayt::entity::Entity*) override;
    void onStart() override;
    const char* getName() const override { return "Combat"; }
    AY_PROPERTY(std::uint64_t, team, ayt::entity::kAttrSerialize)
    AY_PROPERTY(std::uint32_t, capabilities, ayt::entity::kAttrSerialize)
    AY_PROPERTY(bool, invulnerable, ayt::entity::kAttrSerialize)
    AY_PROPERTY(bool, blocking, ayt::entity::kAttrSerialize)
    AY_PROPERTY(std::string, lifeResourceName, ayt::entity::kAttrSerialize)
    std::uint64_t lifeResource=0;
    AY_PROPERTY(std::uint32_t, depletedPhase, ayt::entity::kAttrSerialize)
    AY_PROPERTY(bool, healingRevives, ayt::entity::kAttrSerialize)
    std::vector<std::uint64_t> immuneTypes;
    std::uint64_t generation() const noexcept { return _generation; }
    /// Configure the life mapping once. Internal publication automatically renews Stats incarnations.
    Result<void> configureLife(const ayt::stats::StatsComponent& stats, LifePolicy policy);
    LifeTracker* life() noexcept { return _life ? &*_life : nullptr; }
    const LifeTracker* life() const noexcept { return _life ? &*_life : nullptr; }
    bool lifeMatches(std::uint64_t statsGeneration) const noexcept { return !_life || _lifeStatsGeneration == statsGeneration; }
private:
    std::uint64_t _generation, _lifeStatsGeneration = 0;
    std::optional<LifeTracker> _life;
    LifePolicy _policy;
    ayt::stats::Value _current=0;
    std::optional<LifeTransition> _lastTransition;
    ayt::entity::Entity* _owner=nullptr;
    void synchronizeLife(const ayt::stats::StatsComponent&, ayt::stats::PublicationReason) noexcept;
    friend class CombatSystem;
    friend class StatsPublicationBridge;
};
#undef AY_CURRENT_CLASS
/// World-owned synchronous owner. Simple entries hide the ledger/Stats/version binding machinery.
/// Notifications run on fixed Sim after commit; advanced submit retains explicit outcome access.
class CombatSystem final : public ayt::entity::ISystem {
public:
    CombatSystem();
    const char* getName() const override { return "CombatSystem"; }
    void onUpdate(float) override;
    Result<void> bind(ayt::entity::World& world, ayt::stats::StatsSystem& stats);
    std::uint64_t worldToken() const noexcept { return _token; }
    RelationTable& relations() noexcept { return _relations; }
    HitLedger& hits() noexcept { return _hits; }
    Result<void> setRules(RuleRegistry rules);
    /// Advanced Entity entry: validates current binding/authority, applies current rules,
    /// commits through Stats with next-tick observers, then retains owned outcomes for consumers.
    Result<CombatOutcome> submit(EntityBinding binding, SettlementRequest request,
        const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs);
    std::span<const CombatOutcome> outcomes() const noexcept { return _outcomes; }
    /// Copy/move consumer work starts after authoritative commit. Draining never invokes callbacks.
    std::vector<CombatOutcome> takeOutcomes();
    /// Advanced explicit delivery; normal gameplay installation drains this automatically.
    std::vector<LifeEvent> takeLifeEvents();
    /// Initialize or deliberately rebuild Stats from a schema, then configure the Combat actor.
    /// Existing numerical state and effects are replaced; use attachActor to retain them.
    Result<void> configureActor(ayt::entity::EntityHandle, const ActorProfile&);
    /// Attach Combat to an already configured authoritative StatsComponent without rewriting it.
    /// The life mapping must name a persistent pool in the existing schema.
    Result<void> attachActor(ayt::entity::EntityHandle, LifePolicy, std::uint64_t team,
        std::uint32_t capabilities=mask(Capability::Damageable)|mask(Capability::Healable));
    /// Uses the target's mapped life pool and configured default/explicit Stats recipe.
    /// Permission and actual-value results match advanced submit; each call is a new action.
    Result<ActionResult> damage(ayt::entity::EntityHandle source, ayt::entity::EntityHandle target,
        ayt::stats::Value amount, std::string_view recipe={});
    Result<ActionResult> heal(ayt::entity::EntityHandle source, ayt::entity::EntityHandle target,
        ayt::stats::Value amount, std::string_view recipe={});
    /// Simple generic operations reuse Stats and expose only business values, no version/sync work.
    Result<AttributeResult> changeAttribute(ayt::entity::EntityHandle target,std::string_view stat,
        ayt::stats::AttributeOp operation,ayt::stats::Value amount,std::string_view recipe={},
        std::optional<ayt::entity::EntityHandle> source={});
    Result<AttributeResult> increase(ayt::entity::EntityHandle target,std::string_view stat,ayt::stats::Value amount,std::string_view recipe={}) {
        return changeAttribute(target,stat,ayt::stats::AttributeOp::Increase,amount,recipe);
    }
    Result<AttributeResult> decrease(ayt::entity::EntityHandle target,std::string_view stat,ayt::stats::Value amount,std::string_view recipe={}) {
        return changeAttribute(target,stat,ayt::stats::AttributeOp::Decrease,amount,recipe);
    }
    Result<AttributeResult> setAttribute(ayt::entity::EntityHandle target,std::string_view stat,ayt::stats::Value amount,std::string_view recipe={}) {
        return changeAttribute(target,stat,ayt::stats::AttributeOp::Set,amount,recipe);
    }
    /// Advanced numerical configuration/access. No implicit damage attribution/permission.
    ayt::stats::StatsSystem& attributes() noexcept { return *_stats; }
    Result<void> setRelation(std::uint64_t sourceTeam, std::uint64_t targetTeam, Relation relation);
    /// Simple installation drains notifications automatically, including when no callback is set.
    /// Callbacks cannot destroy the World, components or their owning system. Reentry is rejected.
    void setOnOutcome(std::function<void(const ActionResult&)> callback) { _onOutcome=std::move(callback); }
    void setOnLifeChanged(std::function<void(const LifeEvent&)> callback) { _onLife=std::move(callback); }
    const std::optional<Diagnostic>& lastError() const noexcept { return _error; }
    void automaticDelivery(bool enabled) noexcept { _automatic=enabled; }
private:
    ayt::entity::World* _world = nullptr;
    std::uint64_t _token = 0;
    bool _settling = false;
    bool _delivering = false,_automatic=false;
    ayt::stats::StatsSystem* _stats=nullptr;
    RelationTable _relations;
    HitLedger _hits;
    RuleRegistry _rules;
    std::vector<CombatOutcome> _outcomes;
    static constexpr std::size_t OutcomeLimit = 1024;
    std::vector<LifeEvent> _lifeEvents;
    std::shared_ptr<StatsPublicationBridge> _publication;
    struct ActorRecord { ayt::entity::EntityHandle handle; LifePolicy life; std::uint64_t team; std::uint32_t capabilities; };
    std::vector<ActorRecord> _actors;
    std::function<void(const ActionResult&)> _onOutcome;
    std::function<void(const LifeEvent&)> _onLife;
    std::optional<Diagnostic> _error;
    Result<ActionResult> act(Operation,ayt::entity::EntityHandle,ayt::entity::EntityHandle,ayt::stats::Value,std::string_view);
    void initializeComponent(ayt::entity::EntityHandle,CombatComponent&);
    friend class CombatComponent;
    friend class StatsPublicationBridge;
};
/// Capture a current handle plus World token and component generations; stores no pointers.
Result<EntityBinding> bindCombatant(ayt::entity::World& world, ayt::entity::EntityHandle handle);
/// Register types before sealing the registry, then install after registerStatsSystem(world).
Result<void> registerCombatComponent(ayt::entity::ComponentRegistry& registry);
Result<void> registerCombatSystem(ayt::entity::World& world);
/// Register the complete gameplay dependency set before registry seal.
Result<void> registerCombatGameplayComponents(ayt::entity::ComponentRegistry& registry);
/// Recommended World acquisition. Installs Stats/Combat once and enables automatic delivery.
Result<CombatSystem*> installCombat(ayt::entity::World& world);
/// Startup node includes numerical dependencies; no separate Stats module registration is required.
class CombatIntegrationModule final : public ayt::module::IModule {
public:
    CombatIntegrationModule();
    const ayt::module::ModuleDescriptor& descriptor() const noexcept override { return _descriptor; }
    ayt::module::ModuleResult registerTypes(ayt::module::IModuleContext&) override;
    ayt::module::ModuleResult install(ayt::module::IModuleContext&) override;
private:
    ayt::module::ModuleDescriptor _descriptor;
};
} // namespace ayt::combat
