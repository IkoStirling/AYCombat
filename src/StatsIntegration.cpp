#include <AYCombat/StatsIntegration.h>
#include <algorithm>
#include <exception>
#include <new>
#include <set>
#include <type_traits>

namespace ayt::combat {
namespace {
auto fail(Error code, const char* message) { return std::unexpected(Diagnostic{code, message, {}}); }
Diagnostic numericError(const ayt::stats::Diagnostic& error) {
    const auto code = error.code == ayt::stats::Error::Stale ? Error::Stale :
        (error.code == ayt::stats::Error::Reentrant ? Error::Reentrant :
        (error.code == ayt::stats::Error::Limit ? Error::Limit : Error::NumericFailure));
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
class StatSetTarget final : public SettlementTarget {
public:
    explicit StatSetTarget(ayt::stats::StatSet& state) : _state(state) {}
    Result<const ayt::stats::StatSet*> state() const override { return &_state; }
    ayt::stats::Result<ayt::stats::Settlement> commit(std::span<const ayt::stats::Mutation> mutations, std::uint64_t version) override {
        return _state.commit(mutations, version);
    }
private:
    ayt::stats::StatSet& _state;
};
Result<RuleDecision> ruleDecision(const SettlementRequest& request, SettlementOptions options) {
    if (!options.rules) return RuleDecision{};
    return options.rules->evaluate({request.source, request.target, request.filter.operation, request.damageType, request.facts});
}
}

std::optional<LifeTransition> LifeTracker::observe(const ayt::stats::Settlement& settlement) noexcept {
    bool found = false;
    ayt::stats::Value before = 0, after = 0;
    for (const auto& change : settlement.changes) {
        if (change.target != _policy.resource || change.field != ayt::stats::ChangeField::Current) continue;
        if (!found) before = change.before;
        found = true; after = change.after;
    }
    return found ? observeCurrent(before,after) : std::nullopt;
}
std::optional<LifeTransition> LifeTracker::observeCurrent(ayt::stats::Value before, ayt::stats::Value after) noexcept {
    // Stats commits atomically: an intermediate zero inside one transaction is not death.
    auto next = _phase;
    if (_phase == LifePhase::Alive && before > 0 && after == 0) next = _policy.depleted;
    else if (_phase != LifePhase::Alive && _policy.healingRevives && before == 0 && after > 0) next = LifePhase::Alive;
    if (next != _phase) {
        const LifeTransition transition{_phase, next, ++_sequence}; _phase = next; return transition;
    }
    return std::nullopt;
}

Result<CombatPreview> preview(const SettlementRequest& request, const RelationTable& relations,
                             const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs, SettlementOptions options) {
    auto decision = validate(request, relations);
    if (!decision) return std::unexpected(decision.error());
    if (!decision->allowed) return CombatPreview{*decision};
    auto rule = ruleDecision(request, options);
    if (!rule) return std::unexpected(rule.error());
    if (rule->action != RuleAction::Pass) return CombatPreview{*decision, 0, *rule};
    auto value = evaluate(request, plan, inputs);
    if (!value) return std::unexpected(value.error());
    return CombatPreview{*decision, *value, *rule};
}

Result<CombatOutcome> settle(const SettlementRequest& request, const RelationTable& relations,
                            HitLedger& ledger, ayt::stats::StatSet& targetState,
                            const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs, SettlementOptions options) {
    StatSetTarget target(targetState);
    return settle(request, relations, ledger, target, plan, inputs, options);
}
Result<CombatOutcome> settle(const SettlementRequest& request, const RelationTable& relations,
                            HitLedger& ledger, SettlementTarget& target,
                            const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs, SettlementOptions options) {
    auto decision = validate(request, relations);
    if (!decision) return std::unexpected(decision.error());
    CombatOutcome outcome{request.hit.attack, request.hit.request, request.hit.target, request.source,
                          request.filter.operation, OutcomeState::Rejected, *decision};
    outcome.part=request.hit.part;
    outcome.segment=request.hit.segment;
    if (!decision->allowed) return outcome;
    auto rule = ruleDecision(request, options);
    if (!rule) return std::unexpected(rule.error());
    outcome.rule = *rule;
    if (rule->action == RuleAction::Reject) return outcome;
    if (options.life && (!options.life->policy().resource ||
        (options.life->policy().depleted != LifePhase::Dead && options.life->policy().depleted != LifePhase::Downed)))
        return fail(Error::Invalid, "invalid life resource/depletion policy");
    auto reservation = ledger.reserve(request.hit);
    if (!reservation) return std::unexpected(reservation.error());
    ReservationGuard guard{ledger, *reservation};
    auto boundState = target.state();
    if (!boundState) return std::unexpected(boundState.error());
    if (!*boundState) return fail(Error::Missing, "target state is not configured");
    const auto& targetState = **boundState;
    const auto resourceIndex = targetState.schema()->index(request.resource);
    if (!resourceIndex) return fail(Error::Missing, "target resource is not defined");
    const auto& definition = targetState.schema()->definitions()[*resourceIndex];
    if (definition.shape != ayt::stats::Shape::Pool || definition.lifetime != ayt::stats::Lifetime::Persistent)
        return fail(Error::Invalid, "combat settlement requires a persistent resource pool");
    if (targetState.version() != request.expectedVersion)
        return fail(Error::Stale, "target numerical state changed");
    if (rule->action == RuleAction::Block) {
        if (!ledger.complete(*reservation)) std::terminate();
        guard.completed = true;
        outcome.state = OutcomeState::Applied; outcome.kind = OutcomeKind::Blocked;
        outcome.settlement.beforeVersion = outcome.settlement.afterVersion = request.expectedVersion;
        return outcome;
    }
    auto value = evaluate(request, plan, inputs);
    if (!value) return std::unexpected(value.error());
    const auto op = request.filter.operation == Operation::Damage ? ayt::stats::MutationOp::Decrease : ayt::stats::MutationOp::Increase;
    const ayt::stats::Mutation mutation{request.resource, op, *value};
    auto settlement = target.commit(std::span(&mutation, 1), request.expectedVersion);
    if (!settlement) return std::unexpected(numericError(settlement.error()));
    // No callbacks/allocations after the numerical commit. The pending entry already exists.
    if (!ledger.complete(*reservation)) std::terminate(); // Violation of single-owner/pure-extension contract.
    guard.completed = true;
    outcome.state = OutcomeState::Applied;
    outcome.calculated = *value;
    outcome.settlement = std::move(*settlement);
    outcome.kind = outcome.settlement.changes[0].actual == 0 ? OutcomeKind::Zero :
        (request.filter.operation == Operation::Damage ? OutcomeKind::Damage : OutcomeKind::Heal);
    if (options.life) outcome.life = options.life->observe(outcome.settlement);
    return outcome;
}

Result<CombatOutcome> settleResources(const SettlementRequest& request,
    const RelationTable& relations, HitLedger& ledger,
    ayt::stats::StatSet& targetState,
    std::span<const ayt::stats::Mutation> mutations,
    SettlementOptions options) {
    auto decision = validate(request, relations);
    if (!decision) return std::unexpected(decision.error());
    CombatOutcome outcome{request.hit.attack, request.hit.request, request.hit.target,
                          request.source, request.filter.operation,
                          OutcomeState::Rejected, *decision};
    outcome.part = request.hit.part;
    outcome.segment = request.hit.segment;
    if (!decision->allowed) return outcome;
    auto rule = ruleDecision(request, options);
    if (!rule) return std::unexpected(rule.error());
    outcome.rule = *rule;
    if (rule->action == RuleAction::Reject) return outcome;
    if (options.life && (!options.life->policy().resource ||
        (options.life->policy().depleted != LifePhase::Dead
         && options.life->policy().depleted != LifePhase::Downed)))
        return fail(Error::Invalid, "invalid life resource/depletion policy");
    if (mutations.empty() || mutations.size() > 32)
        return fail(Error::Limit, "resource settlement needs 1..32 mutations");
    if (targetState.version() != request.expectedVersion)
        return fail(Error::Stale, "target numerical state changed");
    std::set<ayt::stats::StatId> seen;
    bool primary = false;
    const auto requiredOp = request.filter.operation == Operation::Damage
        ? ayt::stats::MutationOp::Decrease : ayt::stats::MutationOp::Increase;
    for (const auto& mutation : mutations) {
        if (!mutation.target || mutation.op != requiredOp || mutation.amount < 0
            || !seen.insert(mutation.target).second)
            return fail(Error::Invalid, "invalid or duplicate resource mutation");
        const auto index = targetState.schema()->index(mutation.target);
        if (!index) return fail(Error::Missing, "target resource is not defined");
        const auto& definition = targetState.schema()->definitions()[*index];
        if (definition.shape != ayt::stats::Shape::Pool
            || definition.lifetime != ayt::stats::Lifetime::Persistent)
            return fail(Error::Invalid, "resource mutation needs a persistent pool");
        if (mutation.target == request.resource) {
            primary = true;
            outcome.calculated = mutation.amount;
        }
    }
    if (!primary) return fail(Error::Invalid, "primary resource mutation missing");
    auto reservation = ledger.reserve(request.hit);
    if (!reservation) return std::unexpected(reservation.error());
    ReservationGuard guard{ledger, *reservation};
    if (rule->action == RuleAction::Block) {
        if (!ledger.complete(*reservation)) std::terminate();
        guard.completed = true;
        outcome.state = OutcomeState::Applied;
        outcome.kind = OutcomeKind::Blocked;
        outcome.calculated = 0;
        outcome.settlement.beforeVersion = outcome.settlement.afterVersion
            = request.expectedVersion;
        return outcome;
    }
    auto settlement = targetState.commit(mutations, request.expectedVersion);
    if (!settlement) return std::unexpected(numericError(settlement.error()));
    if (!ledger.complete(*reservation)) std::terminate();
    guard.completed = true;
    outcome.state = OutcomeState::Applied;
    outcome.settlement = std::move(*settlement);
    bool changed = false;
    for (const auto& change : outcome.settlement.changes)
        changed |= change.actual != 0;
    outcome.kind = changed ? (request.filter.operation == Operation::Damage
        ? OutcomeKind::Damage : OutcomeKind::Heal) : OutcomeKind::Zero;
    if (options.life) outcome.life = options.life->observe(outcome.settlement);
    return outcome;
}

Result<PartArmorPlan> planPartArmorHit(const SettlementRequest& request,
    const ayt::stats::StatSet& targetState, const PartArmorRoute& route,
    ayt::stats::Value incoming) {
    using namespace ayt::stats;
    if (request.filter.operation != Operation::Damage || request.hit.part != route.part
        || !route.health || request.resource != route.health
        || route.armor == route.health || incoming < 0
        || route.absorptionBasisPoints < 0
        || route.absorptionBasisPoints > RateScale)
        return fail(Error::Invalid, "invalid part/armor damage route");
    if (request.expectedVersion != targetState.version())
        return fail(Error::Stale, "target numerical state changed");
    const auto healthIndex = targetState.schema()->index(route.health);
    if (!healthIndex) return fail(Error::Missing, "part health pool is missing");
    const auto validPool = [&](std::size_t index) {
        const auto& definition = targetState.schema()->definitions()[index];
        return definition.shape == Shape::Pool
            && definition.lifetime == Lifetime::Persistent;
    };
    if (!validPool(*healthIndex))
        return fail(Error::Invalid, "part health must be a persistent pool");
    PartArmorPlan plan;
    plan.incoming = incoming;
    if (route.armor) {
        const auto armorIndex = targetState.schema()->index(route.armor);
        if (!armorIndex) return fail(Error::Missing, "part armor pool is missing");
        if (!validPool(*armorIndex))
            return fail(Error::Invalid, "part armor must be a persistent pool");
        const auto row = targetState.read(route.armor);
        if (!row) return std::unexpected(numericError(row.error()));
        const auto desired = multiplyDivide(incoming,
            route.absorptionBasisPoints, RateScale);
        if (!desired) return std::unexpected(numericError(desired.error()));
        plan.absorbed = std::min(*desired, row->current);
        plan.mutations[plan.mutationCount++] =
            {route.armor, MutationOp::Decrease, plan.absorbed};
    }
    plan.healthDamage = incoming - plan.absorbed;
    plan.mutations[plan.mutationCount++] =
        {route.health, MutationOp::Decrease, plan.healthDamage};
    return plan;
}

Result<CombatOutcome> settlePartArmorHit(const SettlementRequest& request,
    const RelationTable& relations, HitLedger& ledger,
    ayt::stats::StatSet& targetState, const PartArmorRoute& route,
    ayt::stats::Value incoming, SettlementOptions options) {
    auto plan = planPartArmorHit(request, targetState, route, incoming);
    if (!plan) return std::unexpected(plan.error());
    return settleResources(request, relations, ledger, targetState,
        plan->changes(), options);
}

Result<void> settlePartArmorBatch(std::span<const PartArmorBatchHit> hits,
    std::span<const CombatBatchTarget> targets,
    const RelationTable& relations, HitLedger& ledger,
    std::vector<CombatOutcome>& out, const RuleRegistry* rules) {
    static_assert(std::is_nothrow_move_assignable_v<ayt::stats::StatSet>);
    static_assert(std::is_nothrow_copy_assignable_v<LifeTracker>);
    if (hits.empty() || targets.empty() || hits.size() > 64
        || targets.size() > 64)
        return fail(Error::Limit, "part/armor batch needs 1..64 hits and targets");
    const auto domain = hits.front().request.hit.attack.domain;
    const auto tick = hits.front().request.hit.tick;
    if (!domain) return fail(Error::Invalid, "part/armor batch domain missing");
    for (const auto& hit : hits)
        if (hit.request.hit.tick != tick
            || hit.request.hit.attack.domain != domain)
            return fail(Error::Invalid,
                "part/armor batch hits need one tick and domain");
    struct Shadow {
        CombatantRef ref;
        ayt::stats::StatSet* live;
        ayt::stats::StatSet state;
        LifeTracker* liveLife;
        std::optional<LifeTracker> life;
        std::uint64_t initialVersion;
    };
    try {
        std::vector<Shadow> staged;
        staged.reserve(targets.size());
        for (const auto& target : targets) {
            if (!target.ref.valid() || target.ref.domain != domain
                || !target.state)
                return fail(Error::Invalid, "invalid part/armor batch target");
            for (const auto& prior : staged)
                if (prior.ref == target.ref || prior.live == target.state
                    || (target.life && prior.liveLife == target.life))
                    return fail(Error::Duplicate,
                        "part/armor batch target identity or state duplicated");
            staged.push_back({target.ref, target.state, *target.state,
                target.life, target.life
                    ? std::optional<LifeTracker>(*target.life) : std::nullopt,
                target.state->version()});
        }
        auto stagedLedger = ledger.stagedCopy();
        std::vector<CombatOutcome> outcomes;
        outcomes.reserve(hits.size());
        for (const auto& hit : hits) {
            const auto found = std::find_if(staged.begin(), staged.end(),
                [&](const Shadow& target) {
                    return target.ref == hit.request.hit.target;
                });
            if (found == staged.end())
                return fail(Error::Missing,
                    "part/armor batch target is not bound");
            if (hit.request.expectedVersion != found->initialVersion)
                return fail(Error::Stale,
                    "part/armor batch target version changed before staging");
            auto request = hit.request;
            request.expectedVersion = found->state.version();
            auto plan = planPartArmorHit(request, found->state,
                hit.route, hit.incoming);
            if (!plan) return std::unexpected(plan.error());
            auto settled = settleResources(request, relations, stagedLedger,
                found->state, plan->changes(),
                {rules, found->life ? &*found->life : nullptr});
            if (!settled) return std::unexpected(settled.error());
            outcomes.push_back(std::move(*settled));
        }
        for (auto& target : staged) {
            if (target.state.version() != target.initialVersion)
                *target.live = std::move(target.state);
            if (target.liveLife) *target.liveLife = *target.life;
        }
        ledger = std::move(stagedLedger);
        out.swap(outcomes);
        return {};
    } catch (const std::bad_alloc&) {
        return fail(Error::Limit,
            "part/armor batch candidate allocation failed");
    }
}

Result<void> settleBatch(std::span<const CombatBatchHit> hits,
    std::span<const CombatBatchTarget> targets,
    const RelationTable& relations, HitLedger& ledger,
    std::vector<CombatOutcome>& out, const RuleRegistry* rules) {
    static_assert(std::is_nothrow_move_assignable_v<ayt::stats::StatSet>);
    static_assert(std::is_nothrow_copy_assignable_v<LifeTracker>);
    if (hits.empty() || targets.empty() || hits.size() > 64
        || targets.size() > 64)
        return fail(Error::Limit, "combat batch needs 1..64 hits and targets");
    const auto domain = hits.front().request.hit.attack.domain;
    const auto tick = hits.front().request.hit.tick;
    if (!domain) return fail(Error::Invalid, "combat batch domain is missing");
    for (const auto& hit : hits)
        if (hit.request.hit.tick != tick
            || hit.request.hit.attack.domain != domain)
            return fail(Error::Invalid,
                "combat batch hits must share one tick and domain");
    struct Shadow {
        CombatantRef ref;
        ayt::stats::StatSet* live;
        ayt::stats::StatSet state;
        LifeTracker* liveLife;
        std::optional<LifeTracker> life;
        std::uint64_t initialVersion;
    };
    try {
        std::vector<Shadow> staged;
        staged.reserve(targets.size());
        for (const auto& target : targets) {
            if (!target.ref.valid() || target.ref.domain != domain
                || !target.state)
                return fail(Error::Invalid, "invalid combat batch target");
            for (const auto& prior : staged)
                if (prior.ref == target.ref || prior.live == target.state
                    || (target.life && prior.liveLife == target.life))
                    return fail(Error::Duplicate,
                        "combat batch target identity or state is duplicated");
            staged.push_back({target.ref, target.state, *target.state,
                target.life, target.life
                    ? std::optional<LifeTracker>(*target.life) : std::nullopt,
                target.state->version()});
        }
        auto stagedLedger = ledger.stagedCopy();
        std::vector<CombatOutcome> outcomes;
        outcomes.reserve(hits.size());
        for (const auto& hit : hits) {
            if (!hit.plan)
                return fail(Error::Invalid, "combat batch formula is missing");
            const auto found = std::find_if(staged.begin(), staged.end(),
                [&](const Shadow& target) {
                    return target.ref == hit.request.hit.target;
                });
            if (found == staged.end())
                return fail(Error::Missing, "combat batch target is not bound");
            if (hit.request.expectedVersion != found->initialVersion)
                return fail(Error::Stale,
                    "combat batch target version changed before staging");
            auto request = hit.request;
            request.expectedVersion = found->state.version();
            auto result = settle(request, relations, stagedLedger, found->state,
                *hit.plan, hit.inputs, {rules,
                    found->life ? &*found->life : nullptr});
            if (!result) return std::unexpected(result.error());
            outcomes.push_back(std::move(*result));
        }
        for (auto& target : staged) {
            if (target.state.version() != target.initialVersion)
                *target.live = std::move(target.state);
            if (target.liveLife) *target.liveLife = *target.life;
        }
        ledger = std::move(stagedLedger);
        out.swap(outcomes);
        return {};
    } catch (const std::bad_alloc&) {
        return fail(Error::Limit, "combat batch candidate allocation failed");
    }
}

} // namespace ayt::combat
