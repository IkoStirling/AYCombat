#pragma once
#include <AYCombat/HitLedger.h>
#include <AYCombat/Rules.h>
#include <AYStats/Calculation.h>
#include <array>
#include <functional>

namespace ayt::combat {

/// Synchronous single-StatSet binding; host verifies target identity/binding before call.
/// Relations/target/source snapshots cannot change during this owner-controlled call.
struct SettlementRequest {
    HitRequest hit;
    SourceContext source;
    CombatantView target;
    TargetFilter filter;
    ayt::stats::StatId resource = 0;
    std::uint64_t expectedVersion = 0;
    ayt::stats::CalculationContext calculation;
    std::uint64_t damageType = 0;
    RuleFacts facts;
};
enum class OutcomeState { Rejected, Applied };
enum class OutcomeKind { Rejected, Blocked, Zero, Damage, Heal };
enum class LifePhase { Alive, Downed, Dead };
struct LifeTransition { LifePhase before, after; std::uint64_t sequence; };
struct LifePolicy {
    ayt::stats::StatId resource = 0;
    LifePhase depleted = LifePhase::Dead;
    bool healingRevives = false;
};
struct LifeTrackerState {
    LifePolicy policy;
    LifePhase phase = LifePhase::Alive;
    std::uint64_t sequence = 0;
};
/// Owner-held gameplay lifecycle, separate from Stats pool/effect lifetimes.
/// Observe only committed changes. Policy explicitly chooses death/downed and revival.
class LifeTracker {
public:
    explicit LifeTracker(LifePolicy policy, LifePhase initial = LifePhase::Alive) : _policy(policy), _phase(initial) {}
    const LifePolicy& policy() const noexcept { return _policy; }
    LifePhase phase() const noexcept { return _phase; }
    std::uint64_t sequence() const noexcept { return _sequence; }
    /// The policy is authored content; a saved phase/sequence cannot silently
    /// migrate to a different resource or depletion rule.
    [[nodiscard]] LifeTrackerState snapshot() const noexcept {
        return {_policy, _phase, _sequence};
    }
    [[nodiscard]] bool restore(const LifeTrackerState& state) noexcept {
        if (state.policy.resource != _policy.resource
            || state.policy.depleted != _policy.depleted
            || state.policy.healingRevives != _policy.healingRevives
            || (state.policy.depleted != LifePhase::Downed
                && state.policy.depleted != LifePhase::Dead)
            || (state.phase != LifePhase::Alive
                && state.phase != LifePhase::Downed
                && state.phase != LifePhase::Dead)) return false;
        _phase = state.phase;
        _sequence = state.sequence;
        return true;
    }
    /// Integrations observe final authoritative current even when settlement fields omit clamps.
    std::optional<LifeTransition> observeCurrent(ayt::stats::Value before, ayt::stats::Value after) noexcept;
    std::optional<LifeTransition> observe(const ayt::stats::Settlement& settlement) noexcept;
private:
    LifePolicy _policy;
    LifePhase _phase;
    std::uint64_t _sequence = 0;
};
/// Read-only state access plus the owner's controlled commit entry; never casts away const.
class SettlementTarget {
public:
    virtual ~SettlementTarget() = default;
    virtual Result<const ayt::stats::StatSet*> state() const = 0;
    /// Must preserve Stats atomic commit semantics and return one Change per mutation.
    /// Defer user notifications until the higher-level owner has completed hit/life bookkeeping.
    virtual ayt::stats::Result<ayt::stats::Settlement> commit(std::span<const ayt::stats::Mutation>, std::uint64_t expectedVersion) = 0;
};
struct SettlementOptions { const RuleRegistry* rules = nullptr; LifeTracker* life = nullptr; };
struct CombatOutcome {
    AttackRef attack;
    std::uint64_t request = 0;
    CombatantRef target;
    SourceContext source;
    Operation operation = Operation::Damage;
    OutcomeState state = OutcomeState::Rejected;
    TargetDecision decision;
    ayt::stats::Value calculated = 0;
    ayt::stats::Settlement settlement;
    OutcomeKind kind = OutcomeKind::Rejected;
    RuleDecision rule;
    std::optional<LifeTransition> life;
    /// Entity adapter captures this pool value at action completion, including rejected/blocked work.
    ayt::stats::Value remaining=0;
    /// Physical hit subdivision retained for result consumers and replay diagnostics.
    std::uint32_t part=0,segment=0;
};
struct CombatPreview { TargetDecision decision; ayt::stats::Value calculated = 0; RuleDecision rule; };

/// Pure preview: no reservation, mutation, events or authoritative hit confirmation.
Result<CombatPreview> preview(const SettlementRequest& request, const RelationTable& relations,
                             const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs,
                             SettlementOptions options = {});
/// Recommended minimal combat entry. Formula and resource mutation belong to Stats.
/// Reject leaves allowance available; block and successful zero also confirm the hit.
/// Success completes the reserved identity without allocation after the Stats commit.
/// Numerical failure releases allowance; an applied RequestId can never apply twice.
Result<CombatOutcome> settle(const SettlementRequest& request, const RelationTable& relations,
                            HitLedger& ledger, ayt::stats::StatSet& targetState,
                            const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs,
                            SettlementOptions options = {});
/// Owner adapter overload: all authoritative numerical changes use target.commit.
Result<CombatOutcome> settle(const SettlementRequest& request, const RelationTable& relations,
                            HitLedger& ledger, SettlementTarget& target,
                            const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs,
                            SettlementOptions options = {});

/// Commit an authored set of nonnegative pool changes for one accepted hit.
/// Use this when one part/armor decision changes several pools in the same
/// StatSet. All mutations must match the request's Damage/Heal direction, have
/// distinct persistent pool IDs, and include request.resource as the primary
/// outcome pool. The game computes coverage and amounts before this call.
/// The StatSet commits every mutation atomically or none; one hit receipt is
/// completed only after that commit. Other actors and item durability remain
/// separate game-owned transactions.
Result<CombatOutcome> settleResources(const SettlementRequest& request,
    const RelationTable& relations, HitLedger& ledger,
    ayt::stats::StatSet& targetState,
    std::span<const ayt::stats::Mutation> mutations,
    SettlementOptions options = {});

/// Optional deterministic routing of one part hit through one persistent armor
/// pool and its health pool. The game resolves collider -> part and chooses the
/// absorption rate from its material, ammunition and coverage rules. No random
/// coverage, penetration geometry or Inventory durability is inferred here.
struct PartArmorRoute {
    std::uint32_t part = 0;
    ayt::stats::StatId health = 0;
    ayt::stats::StatId armor = 0; // zero means unarmored
    ayt::stats::Value absorptionBasisPoints = 0; // 0..10000
};
struct PartArmorPlan {
    ayt::stats::Value incoming = 0, absorbed = 0, healthDamage = 0;
    std::array<ayt::stats::Mutation, 2> mutations{};
    std::size_t mutationCount = 0;
    [[nodiscard]] std::span<const ayt::stats::Mutation> changes() const noexcept {
        return {mutations.data(), mutationCount};
    }
};
/// Pure, version-bound preview. Armor absorbs floor(incoming * rate / 10000),
/// capped by its current value; unabsorbed damage reaches health. Errors leave
/// the caller's plan unchanged. Use settlePartArmorHit for one receipt/commit.
Result<PartArmorPlan> planPartArmorHit(const SettlementRequest& request,
    const ayt::stats::StatSet& targetState, const PartArmorRoute& route,
    ayt::stats::Value incoming);
/// One accepted hit publishes both pools atomically through settleResources.
/// A stale/invalid route or numeric failure consumes no hit allowance.
Result<CombatOutcome> settlePartArmorHit(const SettlementRequest& request,
    const RelationTable& relations, HitLedger& ledger,
    ayt::stats::StatSet& targetState, const PartArmorRoute& route,
    ayt::stats::Value incoming, SettlementOptions options = {});

/// One ordered hit in a 1..64-hit same-domain fixed-tick batch. Inputs and plan are
/// borrowed for the call. Every request carries the target's live version
/// at batch start; later hits on that target use staged versions internally.
struct CombatBatchHit {
    SettlementRequest request;
    const ayt::stats::CalculationPlan* plan = nullptr;
    std::span<const ayt::stats::Value> inputs;
};

/// Host rechecks captured World/Entity/component generations after hit query
/// and immediately before any numerical publication. A failure rejects the
/// complete action tick. The callback must be read-only and non-reentrant.
using ActionCombatPreflight = std::function<Result<void>(
    std::span<const CombatBatchHit>)>;

struct CombatBatchTarget {
    CombatantRef ref;
    ayt::stats::StatSet* state = nullptr;
    LifeTracker* life = nullptr;
};

/// Stage hit receipts, all target StatSets and optional life trackers, then
/// publish them together without callbacks or allocations. A failed hit
/// leaves every live owner and out unchanged. The caller orders simultaneous
/// hits and owns action state, Entity events, external effects and persistence.
Result<void> settleBatch(std::span<const CombatBatchHit> hits,
    std::span<const CombatBatchTarget> targets,
    const RelationTable& relations, HitLedger& ledger,
    std::vector<CombatOutcome>& out, const RuleRegistry* rules = nullptr);

} // namespace ayt::combat
