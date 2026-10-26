#pragma once

#include <AYAbility/FixedActionTimeline.h>
#include <AYCombat/StatsIntegration.h>

#include <algorithm>
#include <functional>
#include <new>
#include <type_traits>

namespace ayt::combat {

struct ActionCombatBatch3DResult {
    ayt::ability::FixedActionFrame frame;
    std::vector<CombatOutcome> combat;
};

/// Read-only host query for the active authored hit windows. Return ordered
/// Combat hits with matching tick, execution and window ID as segment. The
/// callback must not mutate timeline, Combat state, World or Entity objects;
/// all borrowed formula/input data must outlive this call.
using ActionCombatHit3DFactory = std::function<Result<std::vector<CombatBatchHit>>(
    const ayt::ability::FixedActionFrame&)>;

/// Advance one 3D action clock and up to 64 Combat hits at one serialized
/// simulation boundary. A failed query, invalid mapping or Combat settlement
/// restores the exact prior timeline checkpoint and leaves numerical state,
/// hit ledger and out unchanged. This uses the timeline's pointer-free
/// checkpoint rather than copying its authored definition table every tick.
/// Physics query, root motion, Entity events and persistence remain host-owned.
[[nodiscard]] inline Result<void> stepAndSettleFixedAction3D(
    ayt::ability::FixedActionTimeline& timeline, std::uint64_t tick,
    const ActionCombatHit3DFactory& makeHits,
    std::span<const CombatBatchTarget> combatTargets,
    const RelationTable& relations, HitLedger& ledger,
    ActionCombatBatch3DResult& out, const RuleRegistry* rules = nullptr) {
    static_assert(std::is_nothrow_move_assignable_v<ayt::ability::FixedActionFrame>);
    const auto before = timeline.checkpoint();
    const auto rollback = [&]() noexcept {
        // This is the unchanged, previously valid checkpoint of this timeline.
        return timeline.restore(before).has_value();
    };
    try {
        auto advanced = timeline.step(tick);
        if (!advanced)
            return std::unexpected(Diagnostic{Error::Invalid,
                advanced.error().message, advanced.error().path});
        auto frame = std::move(*advanced);
        std::vector<CombatBatchHit> hits;
        std::vector<CombatOutcome> combat;
        if (!frame.hitWindows.empty()) {
            if (!makeHits) {
                (void)rollback();
                return std::unexpected(Diagnostic{Error::Invalid,
                    "3D action batch needs a hit query for active windows", {}});
            }
            auto queried = makeHits(frame);
            if (!queried) {
                (void)rollback();
                return std::unexpected(queried.error());
            }
            hits = std::move(*queried);
            if (hits.size() > 64) {
                (void)rollback();
                return std::unexpected(Diagnostic{Error::Limit,
                    "3D action batch exceeds 64 hits", {}});
            }
            for (const auto& hit : hits) {
                const auto window = std::find_if(frame.hitWindows.begin(),
                    frame.hitWindows.end(), [&](const auto& candidate) {
                        return candidate.id == hit.request.hit.segment;
                    });
                if (hit.request.hit.tick != tick
                    || hit.request.source.abilityInstance != frame.execution
                    || window == frame.hitWindows.end()) {
                    (void)rollback();
                    return std::unexpected(Diagnostic{Error::Invalid,
                        "3D Combat hit does not match action tick, execution or window", {}});
                }
            }
            if (!hits.empty()) {
                if (auto settled = settleBatch(hits, combatTargets, relations,
                                               ledger, combat, rules); !settled) {
                    (void)rollback();
                    return std::unexpected(settled.error());
                }
            }
        }
        out.frame = std::move(frame);
        out.combat.swap(combat);
        return {};
    } catch (const std::bad_alloc&) {
        (void)rollback();
        return std::unexpected(Diagnostic{Error::Limit,
            "3D action Combat batch allocation failed", {}});
    } catch (...) {
        (void)rollback();
        return std::unexpected(Diagnostic{Error::Invalid,
            "3D action Combat hit query threw", {}});
    }
}

} // namespace ayt::combat
