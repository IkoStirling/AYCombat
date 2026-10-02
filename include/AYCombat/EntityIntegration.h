#pragma once
#include <AYCombat/StatsIntegration.h>
#include <AYCombat/Interaction.h>
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
/// A denied interaction has no numerical result or effect instance. Source is an owned snapshot.
struct InteractionResult {
    InteractionDecision decision;
    SourceContext source;
    CombatantRef target;
    std::optional<AttributeResult> attribute;
    std::uint64_t effectInstance=0;
};
/// Reusable authored contribution. Duration is measured on the Stats Sim clock;
/// UINT64_MAX means permanent. Modifier identities are allocated by Combat.
enum class LifeRestoreMode { KeepCurrent, RestoreSnapshot, RestoreNetDelta };
struct EffectDefinition {
    std::uint64_t type=0,key=0,durationTicks=UINT64_MAX;
    std::uint32_t maxStacks=UINT32_MAX;
    std::vector<ayt::stats::Modifier> modifiers;
    std::vector<std::uint64_t> tags,immuneEffects,immuneDamageTypes;
    bool stunned=false;
    /// Optional exact life-current rule when a capacity-changing effect ends.
    LifeRestoreMode lifeRestore=LifeRestoreMode::KeepCurrent;
};
/// Entity identity plus both component incarnations. A replacement invalidates queued work.
struct EntityBinding {
    CombatantRef ref;
    std::uint64_t statsGeneration = 0, combatGeneration = 0;
};
/// A headless provider returns current in-range candidates and their selection fact.
struct AreaCandidate { ayt::entity::EntityHandle target; bool selected=false; };
using AreaCandidateProvider = std::function<Result<std::vector<AreaCandidate>>() >;
struct AreaDefinition {
    EffectDefinition effect;
    InteractionPolicy permission;
    std::size_t maxCandidates=1024,maxMembers=1024;
    bool requireTargetAlive=true;
};
enum class AreaTargetState { Entered, Retained, Exited, Rejected, Failed };
struct AreaTargetResult {
    ayt::entity::EntityHandle target;
    AreaTargetState state=AreaTargetState::Rejected;
    std::uint64_t instance=0;
    InteractionDenial denial=InteractionDenial::None;
    std::optional<Diagnostic> error;
};
struct AreaSweep {
    std::uint64_t area=0,tick=0;
    bool ended=false;
    std::vector<AreaTargetResult> targets;
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
    /// Base facts; owned effects contribute separately without modifying these vectors.
    std::vector<std::uint64_t> tags,immuneEffects;
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
    /// Source-aware single-target operation; policy expresses intent independently of value sign.
    /// Candidate selection is supplied by the caller; denial commits nothing.
    Result<InteractionResult> interactAttribute(ayt::entity::EntityHandle source,ayt::entity::EntityHandle target,
        std::string_view stat,ayt::stats::AttributeOp operation,ayt::stats::Value amount,
        InteractionPolicy policy,bool selected=false,std::string_view recipe={});
    /// Advanced derived-hit operation: permission uses captured provenance while optional liveSource
    /// supplies current Stats recipe inputs. A missing liveSource permits snapshot-only recipes.
    Result<InteractionResult> interactAttributeFromContext(const SourceContext& provenance,
        std::optional<ayt::entity::EntityHandle> liveSource,ayt::entity::EntityHandle target,
        std::string_view stat,ayt::stats::AttributeOp operation,ayt::stats::Value amount,
        InteractionPolicy policy,bool selected=false,std::string_view recipe={});
    /// Low-level Stats numerical effect after permission; the caller owns its Stats ID/lifecycle.
    Result<InteractionResult> applyEffect(ayt::entity::EntityHandle source,ayt::entity::EntityHandle target,
        ayt::stats::Effect effect,ayt::stats::EffectPolicy stackPolicy,InteractionPolicy policy,bool selected=false);
    /// Apply one Combat-owned instance. Repeated calls make independent instances up to maxStacks.
    /// Denial creates nothing; successful numerical and status contributions share a completion point.
    Result<InteractionResult> applyOwnedEffect(ayt::entity::EntityHandle source,ayt::entity::EntityHandle target,
        EffectDefinition definition,InteractionPolicy policy,bool selected=false);
    /// Renew only the lifetime, or atomically replace one instance's owned contributions.
    Result<void> refreshOwnedEffect(std::uint64_t instance,std::uint64_t durationTicks);
    Result<void> replaceOwnedEffect(std::uint64_t instance,EffectDefinition definition);
    /// Removal requires a registered instance; unknown IDs return Missing.
    Result<void> removeOwnedEffect(std::uint64_t instance);
    /// Source cancellation also stops its areas, then removes remaining instances;
    /// targets settle independently.
    Result<std::size_t> removeSourceEffects(ayt::entity::EntityHandle source);
    /// Effective facts combine base facts and each live owned contribution.
    Result<bool> hasTag(ayt::entity::EntityHandle target,std::uint64_t tag) const;
    Result<bool> immuneToEffect(ayt::entity::EntityHandle target,std::uint64_t type) const;
    Result<bool> immuneToDamageType(ayt::entity::EntityHandle target,std::uint64_t type) const;
    Result<bool> stunned(ayt::entity::EntityHandle target) const;
    std::size_t ownedEffectCount() const noexcept {return _owned.size();}
    /// N5 headless area: a pure provider enumerates all current in-range candidates.
    /// Fixed Sim rechecks current permission and selection; retained members are not reapplied.
    Result<std::uint64_t> startArea(ayt::entity::EntityHandle source,AreaDefinition definition,
        AreaCandidateProvider provider);
    Result<AreaSweep> updateArea(std::uint64_t area);
    Result<AreaSweep> stopArea(std::uint64_t area);
    Result<AreaSweep> lastAreaSweep(std::uint64_t area) const;
    Result<std::size_t> areaMemberCount(std::uint64_t area) const;
    /// Optional fixed-Sim delivery, including automatically ended areas.
    void setOnAreaSweep(std::function<void(const AreaSweep&)> callback) {_onAreaSweep=std::move(callback);}
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
    struct OwnedEffect {
        std::uint64_t id=0,expires=UINT64_MAX;
        EntityBinding source,target;
        SourceContext provenance;
        EffectDefinition definition;
        struct LifeSnapshot { ayt::stats::StatId resource=0;ayt::stats::Value before=0,after=0; };
        std::optional<LifeSnapshot> lifeSnapshot;
    };
    static constexpr std::size_t OwnedEffectLimit = 65536;
    std::vector<OwnedEffect> _owned;
    std::uint64_t _nextOwnedId=std::uint64_t{1}<<63,_nextModifierHandle=std::uint64_t{1}<<63;
    struct AreaMember { ayt::entity::EntityHandle target; std::uint64_t instance=0; };
    struct AreaRecord {
        std::uint64_t id=0;
        ayt::entity::EntityHandle source;
        EntityBinding sourceBinding;
        AreaDefinition definition;
        AreaCandidateProvider provider;
        std::vector<AreaMember> members;
        AreaSweep last;
    };
    static constexpr std::size_t AreaLimit=1024;
    std::vector<AreaRecord> _areas;
    std::uint64_t _nextAreaId=0;
    std::vector<LifeEvent> _lifeEvents;
    std::shared_ptr<StatsPublicationBridge> _publication;
    struct ActorRecord { ayt::entity::EntityHandle handle; LifePolicy life; std::uint64_t team; std::uint32_t capabilities; };
    std::vector<ActorRecord> _actors;
    std::function<void(const ActionResult&)> _onOutcome;
    std::function<void(const LifeEvent&)> _onLife;
    std::function<void(const AreaSweep&)> _onAreaSweep;
    std::optional<Diagnostic> _error;
    Result<ActionResult> act(Operation,ayt::entity::EntityHandle,ayt::entity::EntityHandle,ayt::stats::Value,std::string_view);
    Result<InteractionResult> authorizeInteraction(ayt::entity::EntityHandle,ayt::entity::EntityHandle,
        InteractionPolicy,bool);
    Result<InteractionResult> authorizeInteractionFromContext(const SourceContext&,
        ayt::entity::EntityHandle,InteractionPolicy,bool);
    bool liveOwned(const OwnedEffect&) const;
    bool hasOwnedTag(const EntityBinding&,std::uint64_t) const;
    bool hasOwnedImmunity(const EntityBinding&,std::uint64_t) const;
    bool hasOwnedDamageImmunity(const EntityBinding&,std::uint64_t) const;
    bool hasOwnedStun(const EntityBinding&) const;
    Result<void> removeOwnedAt(std::size_t);
    void pruneOwnedEffects();
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
