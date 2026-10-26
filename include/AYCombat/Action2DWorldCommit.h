#pragma once

#include <AYApplication/DurableOutbox.h>
#include <AYApplication/WorldStateLedger.h>
#include <AYCombat/Action2DBatch.h>

#include <optional>
#include <memory>
#include <type_traits>

namespace ayt::combat {

enum class Action2DWorldCommitStatus {
    Committed, AlreadyCommitted, Rejected, SaveRejected
};

/// Complete, unpublished fixed-tick state. The game serializes every field it
/// needs, plus other World sections, in one failure-atomic SaveGame slot.
/// Actor/target ordering matches the input spans; no ECS pointers are retained.
struct Action2DWorldCandidate {
    std::vector<std::uint64_t> actorIds;
    std::vector<ayt::ay2d::ActionState2D> actions;
    std::vector<CombatantRef> targetIds;
    std::vector<ayt::stats::StatSet> stats;
    std::vector<std::optional<LifeTracker>> life;
    HitLedgerState hits;
    app::WorldStateLedger world;
    app::DurableOutbox outbox;
    ActionCombatGroup2DResult result;
};

using Action2DWorldSave = std::function<bool(const Action2DWorldCandidate&)>;

/// Versioned, bounded JSON section for one committed candidate. The game may
/// place it beside its other sections in SaveParticipantBundle. Decode checks
/// Stats schemas and HitLedger shape, then the game restores action runtimes
/// against their authored libraries before publishing a staged World.
struct Action2DWorldSnapshot {
    std::vector<std::uint64_t> actorIds;
    std::vector<ayt::ay2d::ActionState2D> actions;
    std::vector<CombatantRef> targetIds;
    std::vector<ayt::stats::StatSet> stats;
    std::vector<std::optional<LifeTrackerState>> life;
    HitLedgerState hits;
    app::WorldStateLedger world;
    app::DurableOutbox outbox;
};
/// Encode one bounded version-1 SaveGame section; leave json unchanged on failure.
[[nodiscard]] bool encodeAction2DWorldCandidate(
    const Action2DWorldCandidate& candidate,
    std::string& json, std::string* error = nullptr);
/// Decode into a temporary, schema-validated value; leave out unchanged on failure.
[[nodiscard]] bool decodeAction2DWorldSnapshot(
    std::string_view json,
    std::span<const std::shared_ptr<const ayt::stats::Schema>> schemas,
    Action2DWorldSnapshot& out, std::string* error = nullptr);

/** Settle one ordinary 2D group tick on private copies, stage a fingerprinted
 * World receipt and durable outcome notices, save the whole candidate, then
 * publish with no allocations. A false/throwing save must leave its old slot
 * intact. The caller serializes World/Entity ownership, captures all live
 * binding generations in preflight, and does not reenter/mutate owners from
 * mapper, preflight or save. External delivery happens after commit through
 * dispatchPendingOutbox; its receiver deduplicates each key. Exact receipt
 * replay does not step or save again. A changed fingerprint is rejected.
 * This does not provide deterministic physics rollback. */
[[nodiscard]] Action2DWorldCommitStatus commitActionGroupWorld2D(
    std::string requestKey, std::string fingerprint,
    std::uint64_t tick, const std::vector<ayt::ay2d::ActionActor2D>& actors,
    const std::function<bool(std::uint64_t, std::uint64_t)>& canHit,
    std::span<const ayt::ay2d::ActionBox2D> blockers,
    std::span<const ayt::ay2d::ActionMovingBlocker2D> movingBlockers,
    const ActionCombatGroupHit2DFactory& makeHit,
    std::span<const CombatBatchTarget> targets,
    const RelationTable& relations, HitLedger& hits,
    app::WorldStateLedger& world, app::DurableOutbox& outbox,
    const Action2DWorldSave& save,
    ActionCombatGroup2DResult& result,
    const RuleRegistry* rules = nullptr,
    const ActionCombatPreflight& preflight = {});

} // namespace ayt::combat
