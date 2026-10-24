#pragma once

#include <AYCombat/EntityIntegration.h>
#include <AY2D/ActionClip2D.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

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

/// Bounded ordinary 2D action-to-Combat settlement receipts. Retrying the
/// same source handle, action cycle and target returns the original result
/// without applying damage again, including an original missing-target or
/// denied result. A changed box, damage or recipe for that receipt is rejected.
/// Keep this beside one live World and clear it on World replacement/restore;
/// prune old cycles only after the game no longer accepts retries for them.
/// This does not make Combat or ActionRuntime2D a deterministic Session owner.
class Action2DCombatReceipts {
public:
    explicit Action2DCombatReceipts(std::size_t maxReceipts = 4096)
        : _maxReceipts(maxReceipts) {}

    template<class ResolveTarget>
    [[nodiscard]] Result<std::vector<Action2DSettlement>> settle(
        CombatSystem& combat, ayt::entity::EntityHandle source,
        const ayt::ay2d::ActionStep2D& step, ResolveTarget&& resolveTarget,
        ayt::stats::Value damage, std::string_view recipe = {})
    {
        if (!source.isValid() || !step.active || !step.cycle || damage <= 0)
            return std::unexpected(Diagnostic{Error::Invalid,
                "2D action settlement needs a live source, cycle and positive damage", {}});
        std::set<std::uint64_t> targets;
        std::map<Key, Receipt> pending;
        for (const auto& hit : step.hits) {
            if (!hit.target || !targets.insert(hit.target).second)
                return std::unexpected(Diagnostic{Error::Invalid,
                    "2D action step contains duplicate or empty targets", {}});
            const Key key{source.id, source.version, step.cycle, hit.target};
            if (const auto found = _receipts.find(key); found != _receipts.end()) {
                if (found->second.hitBox != hit.hitBox
                    || found->second.damage != damage
                    || found->second.recipe != recipe)
                    return std::unexpected(Diagnostic{Error::Duplicate,
                        "2D action retry changed an existing hit", {}});
            } else {
                pending.emplace(key, Receipt{hit.hitBox, damage,
                    std::string(recipe), std::nullopt});
            }
        }
        if (pending.size() > _maxReceipts - std::min(_receipts.size(), _maxReceipts))
            return std::unexpected(Diagnostic{Error::Limit,
                "2D action settlement receipt budget exceeded", {}});
        std::vector<Action2DSettlement> settled;
        settled.reserve(step.hits.size());
        _receipts.merge(pending); // All receipt nodes exist before any Combat mutation.
        for (const auto& hit : step.hits) {
            const Key key{source.id, source.version, step.cycle, hit.target};
            auto& receipt = _receipts.at(key);
            if (!receipt.result) {
                try {
                    const std::optional<ayt::entity::EntityHandle> target =
                        std::invoke(resolveTarget, hit.target);
                    if (!target || !target->isValid())
                        receipt.result.emplace(std::unexpected(Diagnostic{
                            Error::Missing, "2D action target has no live entity", "target"}));
                    else
                        receipt.result.emplace(combat.damage(source, *target,
                            damage, recipe));
                } catch (...) {
                    receipt.result.emplace(std::unexpected(Diagnostic{
                        Error::Stale, "2D action settlement callback threw", {}}));
                }
            }
            settled.push_back({hit.target, hit.hitBox, *receipt.result});
        }
        return settled;
    }

    /// The caller promises that older cycles for this source cannot be retried.
    void forgetBefore(ayt::entity::EntityHandle source, std::uint64_t firstCycle)
    {
        for (auto it = _receipts.begin(); it != _receipts.end();)
            if (std::get<0>(it->first) == source.id
                && std::get<1>(it->first) == source.version
                && std::get<2>(it->first) < firstCycle)
                it = _receipts.erase(it);
            else ++it;
    }
    void clear() noexcept { _receipts.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return _receipts.size(); }

private:
    using Key = std::tuple<std::uint32_t, std::uint32_t,
                           std::uint64_t, std::uint64_t>;
    struct Receipt {
        std::uint32_t hitBox;
        ayt::stats::Value damage;
        std::string recipe;
        std::optional<Result<ActionResult>> result;
    };
    std::size_t _maxReceipts;
    std::map<Key, Receipt> _receipts;
};

} // namespace ayt::combat
