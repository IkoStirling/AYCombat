#pragma once

#include <AYApplication/DurableOutbox.h>
#include <AYApplication/WorldStateLedger.h>
#include <AYCombat/Action3DBatch.h>

#include <memory>
#include <optional>

namespace ayt::combat {

enum class Action3DWorldCommitStatus {
    Committed, AlreadyCommitted, Rejected, SaveRejected
};

/// Private, complete numerical candidate for one 3D action tick. The game
/// saves this with its authoritative Entity, animation and physics sections.
struct Action3DWorldCandidate {
    ayt::ability::FixedActionSnapshot action;
    std::vector<CombatantRef> targetIds;
    std::vector<ayt::stats::StatSet> stats;
    std::vector<std::optional<LifeTracker>> life;
    HitLedgerState hits;
    app::WorldStateLedger world;
    app::DurableOutbox outbox;
    ActionCombatBatch3DResult result;
};

using Action3DWorldSave = std::function<bool(const Action3DWorldCandidate&)>;

struct Action3DWorldSnapshot {
    ayt::ability::FixedActionSnapshot action;
    std::vector<CombatantRef> targetIds;
    std::vector<ayt::stats::StatSet> stats;
    std::vector<std::optional<LifeTrackerState>> life;
    HitLedgerState hits;
    app::WorldStateLedger world;
    app::DurableOutbox outbox;
};

/// Encode one bounded version-1 section; leave json unchanged on failure.
[[nodiscard]] bool encodeAction3DWorldCandidate(
    const Action3DWorldCandidate& candidate,
    std::string& json, std::string* error = nullptr);
/// Decode into a validated temporary; game restores against authored timeline.
[[nodiscard]] bool decodeAction3DWorldSnapshot(
    std::string_view json,
    std::span<const std::shared_ptr<const ayt::stats::Schema>> schemas,
    Action3DWorldSnapshot& out, std::string* error = nullptr);

/** Advance a private copy of the previewed timeline and Combat owners, then
 * save the complete candidate before publishing values. Exact receipt replay
 * does not requery or resave; a different fingerprint conflicts. The game
 * keeps this simulation boundary exclusive, checks captured Entity/component
 * generations in preflight, and saves other World/physics state in the same
 * failure-atomic slot. Deliver outbox entries only after this returns Committed.
 */
[[nodiscard]] Action3DWorldCommitStatus commitActionWorld3D(
    std::string requestKey, std::string fingerprint,
    ayt::ability::FixedActionTimeline& timeline, std::uint64_t tick,
    const ayt::ability::FixedActionSnapshot& expected,
    const ActionCombatHit3DFactory& makeHits,
    std::span<const CombatBatchTarget> targets,
    const RelationTable& relations, HitLedger& hits,
    app::WorldStateLedger& world, app::DurableOutbox& outbox,
    const Action3DWorldSave& save,
    ActionCombatBatch3DResult& result,
    const RuleRegistry* rules = nullptr,
    const ActionCombatPreflight& preflight = {});

} // namespace ayt::combat
