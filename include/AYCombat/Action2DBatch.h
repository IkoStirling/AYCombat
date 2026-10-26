#pragma once

#include <AY2D/ActionClip2D.h>
#include <AYCombat/StatsIntegration.h>

#include <functional>
#include <new>
#include <type_traits>

namespace ayt::combat {

struct ActionCombatBatch2DResult {
    ayt::ay2d::ActionStep2D action;
    std::vector<CombatOutcome> combat;
};

/// Pure game-owned mapping from one action hit to its Combat request and
/// borrowed formula/inputs. The returned plan and input span must outlive
/// this call; the request must use the supplied action tick. The mapper must
/// not mutate the action runtime, ledger, target states or life trackers.
using ActionCombatHit2DFactory = std::function<Result<CombatBatchHit>(
    const ayt::ay2d::ActionHit2D&, const ayt::ay2d::ActionStep2D&)>;

/// Advance one ordinary 2D action path and its 1..64 low-level Combat hits
/// at one serialized simulation boundary. A missing/invalid mapping or failed
/// Combat batch leaves action time, StatSets, life, ledger and out unchanged.
/// For multiple authored hit windows, the game maps ActionStep2D::hitWindow
/// to HitRequest::segment under a Segment-scope attack and uses unique requests.
/// Optional fixed-shape moving blockers use the same actual path contract as
/// ActionRuntime2D::stepPath and are staged with the action/combat candidate.
/// Zero action hits advance the action only. Combat hits retain ActionStep2D
/// order; the game owns target identity mapping, World/Entity effects and
/// persistence.
[[nodiscard]] inline Result<void> stepAndSettleActionPath2D(
    ayt::ay2d::ActionRuntime2D& runtime, std::uint64_t tick,
    const ayt::ay2d::ActionInput2D& input,
    const std::vector<ayt::ay2d::ActionPathPoint2D>& ownerPath,
    const std::vector<ayt::ay2d::ActionTarget2D>& actionTargets,
    std::span<const ayt::ay2d::ActionBox2D> blockers,
    std::span<const ayt::ay2d::ActionMovingBlocker2D> movingBlockers,
    const ActionCombatHit2DFactory& makeHit,
    std::span<const CombatBatchTarget> combatTargets,
    const RelationTable& relations, HitLedger& ledger,
    ActionCombatBatch2DResult& out, const RuleRegistry* rules = nullptr,
    const ActionCombatPreflight& preflight = {}) {
    static_assert(std::is_nothrow_move_assignable_v<ayt::ay2d::ActionRuntime2D>);
    static_assert(std::is_nothrow_move_assignable_v<ayt::ay2d::ActionStep2D>);
    try {
        auto candidate = runtime;
        ayt::ay2d::ActionStep2D action;
        if (!candidate.stepPath(tick, input, ownerPath, actionTargets,
                                action, blockers, movingBlockers))
            return std::unexpected(Diagnostic{Error::Invalid,
                "2D action batch path or tick was rejected", {}});
        if (action.hits.size() > 64)
            return std::unexpected(Diagnostic{Error::Limit,
                "2D action batch exceeds 64 hits", {}});
        std::vector<CombatBatchHit> hits;
        std::vector<CombatOutcome> combat;
        if (!action.hits.empty()) {
            if (!makeHit)
                return std::unexpected(Diagnostic{Error::Invalid,
                    "2D action batch needs a Combat hit mapper", {}});
            hits.reserve(action.hits.size());
            for (const auto& actionHit : action.hits) {
                auto bound = makeHit(actionHit, action);
                if (!bound) return std::unexpected(bound.error());
                if (bound->request.hit.tick != tick)
                    return std::unexpected(Diagnostic{Error::Invalid,
                        "2D action Combat hit uses another tick", {}});
                hits.push_back(std::move(*bound));
            }
            if (preflight) {
                auto checked = preflight(hits);
                if (!checked) return std::unexpected(checked.error());
            }
            if (auto settled = settleBatch(hits, combatTargets, relations,
                                           ledger, combat, rules); !settled)
                return std::unexpected(settled.error());
        }
        if (action.hits.empty() && preflight) {
            auto checked = preflight(hits);
            if (!checked) return std::unexpected(checked.error());
        }
        runtime = std::move(candidate);
        out.action = std::move(action);
        out.combat.swap(combat);
        return {};
    } catch (const std::bad_alloc&) {
        return std::unexpected(Diagnostic{Error::Limit,
            "2D action Combat batch allocation failed", {}});
    } catch (...) {
        return std::unexpected(Diagnostic{Error::Invalid,
            "2D action Combat hit mapper threw", {}});
    }
}

/// Compatibility overload for callers with only static blockers.
[[nodiscard]] inline Result<void> stepAndSettleActionPath2D(
    ayt::ay2d::ActionRuntime2D& runtime, std::uint64_t tick,
    const ayt::ay2d::ActionInput2D& input,
    const std::vector<ayt::ay2d::ActionPathPoint2D>& ownerPath,
    const std::vector<ayt::ay2d::ActionTarget2D>& actionTargets,
    std::span<const ayt::ay2d::ActionBox2D> blockers,
    const ActionCombatHit2DFactory& makeHit,
    std::span<const CombatBatchTarget> combatTargets,
    const RelationTable& relations, HitLedger& ledger,
    ActionCombatBatch2DResult& out, const RuleRegistry* rules = nullptr,
    const ActionCombatPreflight& preflight = {}) {
    return stepAndSettleActionPath2D(runtime, tick, input, ownerPath,
        actionTargets, blockers, {}, makeHit, combatTargets, relations,
        ledger, out, rules, preflight);
}

struct ActionCombatGroup2DResult {
    std::vector<ayt::ay2d::ActionStep2D> actions;
    std::vector<CombatOutcome> combat;
};

/// Pure mapping for a hit in one staged actor's action result. Sources are
/// visited in ascending ActionActor2D ID order, then in that step's hit order.
using ActionCombatGroupHit2DFactory = std::function<Result<CombatBatchHit>(
    std::uint64_t sourceActorId, const ayt::ay2d::ActionHit2D&,
    const ayt::ay2d::ActionStep2D&)>;

/// Stage all 1..64 ordinary actor actions and their combined low-level Combat
/// batch before publication. A failed actor, mapper or Combat hit leaves all
/// live runtimes, StatSets, life trackers, ledger and out unchanged. The game
/// supplies the hit mapping, serialized ownership and external persistence.
[[nodiscard]] inline Result<void> stepAndSettleActionGroup2D(
    std::uint64_t tick, const std::vector<ayt::ay2d::ActionActor2D>& actors,
    const std::function<bool(std::uint64_t, std::uint64_t)>& canHit,
    std::span<const ayt::ay2d::ActionBox2D> blockers,
    std::span<const ayt::ay2d::ActionMovingBlocker2D> movingBlockers,
    const ActionCombatGroupHit2DFactory& makeHit,
    std::span<const CombatBatchTarget> combatTargets,
    const RelationTable& relations, HitLedger& ledger,
    ActionCombatGroup2DResult& out, const RuleRegistry* rules = nullptr,
    const ActionCombatPreflight& preflight = {}) {
    static_assert(std::is_nothrow_move_assignable_v<ayt::ay2d::ActionRuntime2D>);
    if (actors.empty() || actors.size() > 64)
        return std::unexpected(Diagnostic{Error::Limit,
            "2D action Combat group needs 1..64 actors", {}});
    for (std::size_t i = 0; i < actors.size(); ++i) {
        if (!actors[i].runtime)
            return std::unexpected(Diagnostic{Error::Invalid,
                "2D action Combat group has a null runtime", {}});
        for (std::size_t j = 0; j < i; ++j)
            if (actors[i].runtime == actors[j].runtime)
                return std::unexpected(Diagnostic{Error::Duplicate,
                    "2D action Combat group repeats a runtime", {}});
    }
    try {
        std::vector<ayt::ay2d::ActionRuntime2D> candidates;
        candidates.reserve(actors.size());
        for (const auto& actor : actors)
            candidates.push_back(*actor.runtime);
        auto stagedActors = actors;
        for (std::size_t i = 0; i < actors.size(); ++i)
            stagedActors[i].runtime = &candidates[i];
        std::vector<ayt::ay2d::ActionStep2D> actions;
        if (!ayt::ay2d::stepActionGroup2D(tick, stagedActors, actions,
                                          canHit, blockers, movingBlockers))
            return std::unexpected(Diagnostic{Error::Invalid,
                "2D action Combat group step was rejected", {}});
        std::size_t hitCount = 0;
        for (const auto& action : actions) {
            hitCount += action.hits.size();
            if (hitCount > 64)
                return std::unexpected(Diagnostic{Error::Limit,
                    "2D action Combat group exceeds 64 hits", {}});
        }
        std::vector<CombatBatchHit> hits;
        std::vector<CombatOutcome> combat;
        if (hitCount) {
            if (!makeHit)
                return std::unexpected(Diagnostic{Error::Invalid,
                    "2D action Combat group needs a hit mapper", {}});
            hits.reserve(hitCount);
            for (std::size_t i = 0; i < actors.size(); ++i) {
                for (const auto& actionHit : actions[i].hits) {
                    auto bound = makeHit(actors[i].id, actionHit, actions[i]);
                    if (!bound) return std::unexpected(bound.error());
                    if (bound->request.hit.tick != tick)
                        return std::unexpected(Diagnostic{Error::Invalid,
                            "2D action group Combat hit uses another tick", {}});
                    hits.push_back(std::move(*bound));
                }
            }
            if (preflight) {
                auto checked = preflight(hits);
                if (!checked) return std::unexpected(checked.error());
            }
            if (auto settled = settleBatch(hits, combatTargets, relations,
                                           ledger, combat, rules); !settled)
                return std::unexpected(settled.error());
        }
        if (!hitCount && preflight) {
            auto checked = preflight(hits);
            if (!checked) return std::unexpected(checked.error());
        }
        for (std::size_t i = 0; i < actors.size(); ++i)
            *actors[i].runtime = std::move(candidates[i]);
        out.actions.swap(actions);
        out.combat.swap(combat);
        return {};
    } catch (const std::bad_alloc&) {
        return std::unexpected(Diagnostic{Error::Limit,
            "2D action Combat group allocation failed", {}});
    } catch (...) {
        return std::unexpected(Diagnostic{Error::Invalid,
            "2D action Combat group callback threw", {}});
    }
}

/// Compatibility overload for callers with only static blockers.
[[nodiscard]] inline Result<void> stepAndSettleActionGroup2D(
    std::uint64_t tick, const std::vector<ayt::ay2d::ActionActor2D>& actors,
    const std::function<bool(std::uint64_t, std::uint64_t)>& canHit,
    std::span<const ayt::ay2d::ActionBox2D> blockers,
    const ActionCombatGroupHit2DFactory& makeHit,
    std::span<const CombatBatchTarget> combatTargets,
    const RelationTable& relations, HitLedger& ledger,
    ActionCombatGroup2DResult& out, const RuleRegistry* rules = nullptr,
    const ActionCombatPreflight& preflight = {}) {
    return stepAndSettleActionGroup2D(tick, actors, canHit, blockers, {},
        makeHit, combatTargets, relations, ledger, out, rules, preflight);
}

} // namespace ayt::combat
