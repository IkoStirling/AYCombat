#pragma once
#include <AYCombat/HitLedger.h>
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
};
enum class OutcomeState { Rejected, Applied };
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
};
struct CombatPreview { TargetDecision decision; ayt::stats::Value calculated = 0; };

/// Pure preview: no reservation, mutation, events or authoritative hit confirmation.
Result<CombatPreview> preview(const SettlementRequest& request, const RelationTable& relations,
                             const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs);
/// Recommended minimal combat entry. Formula and resource mutation belong to Stats.
/// Success completes the reserved identity without allocation after the Stats commit.
/// Numerical failure releases allowance; an applied RequestId can never apply twice.
Result<CombatOutcome> settle(const SettlementRequest& request, const RelationTable& relations,
                            HitLedger& ledger, ayt::stats::StatSet& targetState,
                            const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs);

} // namespace ayt::combat
