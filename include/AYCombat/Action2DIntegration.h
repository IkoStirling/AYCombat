#pragma once

#include <AYCombat/EntityIntegration.h>
#include <AY2D/ActionClip2D.h>

#include <functional>
#include <optional>
#include <string_view>

namespace ayt::combat {

/// One completed ordinary 2D action hit and its individual Combat result.
/// Results are ordered like ActionStep2D::hits; a later failure does not undo
/// earlier settlements. The action runtime already deduplicates each target
/// within one cycle, so callers must dispatch a completed step only once.
struct Action2DSettlement {
    std::uint64_t targetId = 0;
    std::uint32_t hitBox = 0;
    Result<ActionResult> result;
};

/// Route authored 2D hit facts through the World-owned Combat facade. The
/// resolver maps stable action target IDs to current EntityHandles; stale or
/// missing mappings are reported per hit. Geometry, team permission and HP
/// remain owned by ActionRuntime2D and Combat respectively. This ordinary
/// floating-point route is not a deterministic Session/rollback adapter.
template<class ResolveTarget>
[[nodiscard]] std::vector<Action2DSettlement> dispatchActionHits2D(
    CombatSystem& combat, ayt::entity::EntityHandle source,
    const ayt::ay2d::ActionStep2D& step, ResolveTarget&& resolveTarget,
    ayt::stats::Value damage, std::string_view recipe = {})
{
    std::vector<Action2DSettlement> settled;
    settled.reserve(step.hits.size());
    for (const auto& hit : step.hits) {
        const std::optional<ayt::entity::EntityHandle> target =
            std::invoke(resolveTarget, hit.target);
        if (!target || !target->isValid()) {
            settled.push_back({hit.target, hit.hitBox,
                std::unexpected(Diagnostic{Error::Missing,
                    "2D action target has no live entity", "target"})});
            continue;
        }
        settled.push_back({hit.target, hit.hitBox,
            combat.damage(source, *target, damage, recipe)});
    }
    return settled;
}

} // namespace ayt::combat
