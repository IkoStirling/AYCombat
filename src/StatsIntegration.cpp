#include <AYCombat/StatsIntegration.h>
#include <exception>

namespace ayt::combat {
namespace {
auto fail(Error code, const char* message) { return std::unexpected(Diagnostic{code, message, {}}); }
Diagnostic numericError(const ayt::stats::Diagnostic& error) {
    const auto code = error.code == ayt::stats::Error::Stale ? Error::Stale : Error::NumericFailure;
    return {code, error.message, error.path};
}
Result<TargetDecision> validate(const SettlementRequest& request, const RelationTable& relations) {
    if (request.target.ref != request.hit.target || !request.hit.attack.valid() ||
        request.hit.attack.domain != request.target.ref.domain || !request.hit.request || !request.resource ||
        !request.calculation.calculation || !request.calculation.hit || request.calculation.tick != request.hit.tick)
        return fail(Error::Invalid, "invalid settlement binding or calculation identity/tick");
    return filterTarget(request.source, request.target, relations, request.filter);
}
Result<ayt::stats::Value> evaluate(const SettlementRequest& request, const ayt::stats::CalculationPlan& plan,
                                  std::span<const ayt::stats::Value> inputs) {
    std::vector<ayt::stats::Value> scratch(plan.instructions().size());
    auto value = plan.evaluate(inputs, request.calculation, scratch);
    if (!value) return std::unexpected(numericError(value.error()));
    if (*value < 0) return fail(Error::Invalid, "negative damage/healing amount is not allowed");
    return *value;
}
struct ReservationGuard {
    HitLedger& ledger;
    Reservation reservation;
    bool completed = false;
    ~ReservationGuard() { if (!completed) ledger.release(reservation); }
};
}

Result<CombatPreview> preview(const SettlementRequest& request, const RelationTable& relations,
                             const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs) {
    auto decision = validate(request, relations);
    if (!decision) return std::unexpected(decision.error());
    if (!decision->allowed) return CombatPreview{*decision};
    auto value = evaluate(request, plan, inputs);
    if (!value) return std::unexpected(value.error());
    return CombatPreview{*decision, *value};
}

Result<CombatOutcome> settle(const SettlementRequest& request, const RelationTable& relations,
                            HitLedger& ledger, ayt::stats::StatSet& targetState,
                            const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs) {
    auto decision = validate(request, relations);
    if (!decision) return std::unexpected(decision.error());
    CombatOutcome outcome{request.hit.attack, request.hit.request, request.hit.target, request.source,
                          request.filter.operation, OutcomeState::Rejected, *decision};
    if (!decision->allowed) return outcome;
    auto reservation = ledger.reserve(request.hit);
    if (!reservation) return std::unexpected(reservation.error());
    ReservationGuard guard{ledger, *reservation};
    const auto resourceIndex = targetState.schema()->index(request.resource);
    if (!resourceIndex) return fail(Error::Missing, "target resource is not defined");
    const auto& definition = targetState.schema()->definitions()[*resourceIndex];
    if (definition.shape != ayt::stats::Shape::Pool || definition.lifetime != ayt::stats::Lifetime::Persistent)
        return fail(Error::Invalid, "combat settlement requires a persistent resource pool");
    if (targetState.version() != request.expectedVersion)
        return fail(Error::Stale, "target numerical state changed");
    auto value = evaluate(request, plan, inputs);
    if (!value) return std::unexpected(value.error());
    const auto op = request.filter.operation == Operation::Damage ? ayt::stats::MutationOp::Decrease : ayt::stats::MutationOp::Increase;
    const ayt::stats::Mutation mutation{request.resource, op, *value};
    auto settlement = targetState.commit(std::span(&mutation, 1), request.expectedVersion);
    if (!settlement) return std::unexpected(numericError(settlement.error()));
    // No callbacks/allocations after the numerical commit. The pending entry already exists.
    if (!ledger.complete(*reservation)) std::terminate(); // Violation of single-owner/pure-extension contract.
    guard.completed = true;
    outcome.state = OutcomeState::Applied;
    outcome.calculated = *value;
    outcome.settlement = std::move(*settlement);
    return outcome;
}

} // namespace ayt::combat
