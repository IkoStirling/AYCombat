#pragma once
#include <AYCombat/HitLedger.h>
#include <AYCombat/Rules.h>
#include <AYStats/Calculation.h>

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
/// Owner-held gameplay lifecycle, separate from Stats pool/effect lifetimes.
/// Observe only committed changes. Policy explicitly chooses death/downed and revival.
class LifeTracker {
public:
    explicit LifeTracker(LifePolicy policy, LifePhase initial = LifePhase::Alive) : _policy(policy), _phase(initial) {}
    const LifePolicy& policy() const noexcept { return _policy; }
    LifePhase phase() const noexcept { return _phase; }
    std::uint64_t sequence() const noexcept { return _sequence; }
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

} // namespace ayt::combat
