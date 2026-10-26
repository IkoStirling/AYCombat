#include <AYCombat/Action3DWorldCommit.h>

#include <new>
#include <type_traits>

namespace ayt::combat {
namespace {
std::string noticeKey(const CombatOutcome& outcome) {
    return "combat/" + std::to_string(outcome.attack.domain) + "/"
        + std::to_string(outcome.attack.id) + "/"
        + std::to_string(outcome.request);
}
std::string noticeFingerprint(const CombatOutcome& outcome) {
    return std::to_string(outcome.target.domain) + "/"
        + std::to_string(outcome.target.object) + "/"
        + std::to_string(outcome.target.generation) + "/"
        + std::to_string(static_cast<int>(outcome.kind)) + "/"
        + std::to_string(outcome.calculated) + "/"
        + std::to_string(outcome.remaining) + "/"
        + std::to_string(outcome.life ? outcome.life->sequence : 0);
}
std::string noticePayload(const CombatOutcome& outcome) {
    return "{\"type\":\"combat.outcome.v1\",\"domain\":"
        + std::to_string(outcome.target.domain) + ",\"target\":"
        + std::to_string(outcome.target.object) + ",\"generation\":"
        + std::to_string(outcome.target.generation) + ",\"kind\":"
        + std::to_string(static_cast<int>(outcome.kind)) + ",\"amount\":"
        + std::to_string(outcome.calculated) + ",\"remaining\":"
        + std::to_string(outcome.remaining) + ",\"lifeAfter\":"
        + std::to_string(outcome.life
            ? static_cast<int>(outcome.life->after) : -1) + "}";
}
} // namespace

Action3DWorldCommitStatus commitActionWorld3D(
    std::string requestKey, std::string fingerprint,
    ayt::ability::FixedActionTimeline& timeline, std::uint64_t tick,
    const ayt::ability::FixedActionSnapshot& expected,
    const ActionCombatHit3DFactory& makeHits,
    std::span<const CombatBatchTarget> targets,
    const RelationTable& relations, HitLedger& hits,
    app::WorldStateLedger& world, app::DurableOutbox& outbox,
    const Action3DWorldSave& save,
    ActionCombatBatch3DResult& result,
    const RuleRegistry* rules,
    const ActionCombatPreflight& preflight) {
    static_assert(std::is_nothrow_move_assignable_v<ayt::ability::FixedActionTimeline>);
    static_assert(std::is_nothrow_move_assignable_v<ayt::stats::StatSet>);
    static_assert(std::is_nothrow_move_assignable_v<HitLedger>);
    static_assert(std::is_nothrow_copy_assignable_v<LifeTracker>);
    static_assert(std::is_nothrow_move_assignable_v<ayt::ability::FixedActionFrame>);
    if (!save || targets.size() > 64)
        return Action3DWorldCommitStatus::Rejected;
    for (const auto& target : targets)
        if (!target.state) return Action3DWorldCommitStatus::Rejected;
    try {
        Action3DWorldCandidate candidate;
        candidate.world = world;
        candidate.outbox = outbox;
        const auto claimed = candidate.world.claimReceiptWithFingerprint(
            std::move(requestKey), std::move(fingerprint));
        if (claimed == app::WorldStateLedger::ReceiptClaimResult::Replay)
            return Action3DWorldCommitStatus::AlreadyCommitted;
        if (claimed != app::WorldStateLedger::ReceiptClaimResult::Claimed
            || timeline.checkpoint() != expected)
            return Action3DWorldCommitStatus::Rejected;

        auto stagedTimeline = timeline;
        candidate.targetIds.reserve(targets.size());
        candidate.stats.reserve(targets.size());
        candidate.life.reserve(targets.size());
        for (const auto& target : targets) {
            candidate.targetIds.push_back(target.ref);
            candidate.stats.push_back(*target.state);
            if (target.life) candidate.life.emplace_back(*target.life);
            else candidate.life.emplace_back(std::nullopt);
        }
        std::vector<CombatBatchTarget> targetBindings;
        targetBindings.reserve(targets.size());
        for (std::size_t i = 0; i < targets.size(); ++i)
            targetBindings.push_back({targets[i].ref, &candidate.stats[i],
                candidate.life[i] ? &*candidate.life[i] : nullptr});
        auto stagedHits = hits.stagedCopy();
        if (!stepAndSettleFixedAction3D(stagedTimeline, tick, expected, makeHits,
                targetBindings, relations, stagedHits, candidate.result, rules,
                preflight))
            return Action3DWorldCommitStatus::Rejected;
        candidate.action = stagedTimeline.checkpoint();
        if (!stagedHits.snapshot(candidate.hits))
            return Action3DWorldCommitStatus::Rejected;
        for (const auto& outcome : candidate.result.combat) {
            const auto queued = candidate.outbox.enqueue(noticeKey(outcome),
                noticeFingerprint(outcome), noticePayload(outcome));
            if (queued != app::OutboxEnqueueResult::Enqueued)
                return Action3DWorldCommitStatus::Rejected;
        }
        try {
            if (!save(candidate)) return Action3DWorldCommitStatus::SaveRejected;
        } catch (...) {
            return Action3DWorldCommitStatus::SaveRejected;
        }
        timeline = std::move(stagedTimeline);
        for (std::size_t i = 0; i < targets.size(); ++i) {
            *targets[i].state = std::move(candidate.stats[i]);
            if (targets[i].life) *targets[i].life = *candidate.life[i];
        }
        hits = std::move(stagedHits);
        world.swap(candidate.world);
        outbox.swap(candidate.outbox);
        result.frame = std::move(candidate.result.frame);
        result.combat.swap(candidate.result.combat);
        return Action3DWorldCommitStatus::Committed;
    } catch (const std::bad_alloc&) {
        return Action3DWorldCommitStatus::Rejected;
    } catch (...) {
        return Action3DWorldCommitStatus::Rejected;
    }
}

} // namespace ayt::combat
