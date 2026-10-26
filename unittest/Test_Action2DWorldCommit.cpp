#include <AYTest.h>
#include <AYCombat/Action2DWorldCommit.h>
#include <AYApplication/SaveGameService.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <set>

using namespace ayt::combat;

TEST_SUITE(Action2DWorldCommitTests)

TEST_CASE(two_attackers_save_before_publish_and_retry_without_second_damage) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ay-action-world-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
    ayt::app::SaveGameService saveGame({root.string(), "combat-world-test"});
    ayt::ay2d::ActionFrame2D strike;
    strike.hitBoxes = {{0.2f, -0.2f, 1.0f, 0.2f}};
    strike.hurtBoxes = {{-0.2f, -0.5f, 0.2f, 0.5f}};
    ayt::ay2d::ActionFrame2D idle;
    idle.hurtBoxes = strike.hurtBoxes;
    const ayt::ay2d::ActionLibrary2D clips{{
        {"strike", {strike}, true}, {"idle", {idle}, true}}};
    ayt::ay2d::ActionRuntime2D first(clips), second(clips), victim(clips);
    const std::vector<ayt::ay2d::ActionActor2D> actors{
        {1, &first, {"strike", true}, 0.0f, 0.0f},
        {2, &second, {"strike", true}, 0.0f, 0.0f},
        {3, &victim, {"idle", true}, 0.6f, 0.0f}};
    const auto schema = ayt::stats::Schema::compile({{
        .name="health", .shape=ayt::stats::Shape::Pool,
        .current=100, .capacity=100}}).value();
    auto health = ayt::stats::StatSet::create(schema).value();
    LifeTracker life({ayt::stats::statId("health"), LifePhase::Dead});
    const std::array<CombatBatchTarget, 1> targets{{
        {{1, 3, 1}, &health, &life}}};
    HitLedger hits;
    CHECK_TRUE(hits.open({1, 1}, {}, 0, 100).has_value());
    CHECK_TRUE(hits.open({1, 2}, {}, 0, 100).has_value());
    RelationTable relations;
    CHECK_TRUE(relations.set(10, 20, Relation::Hostile).has_value());
    const auto formula = ayt::stats::CalculationPlan::compile({{
        .id=1, .op=ayt::stats::Op::Input, .binding=0}}, 1, 1).value();
    const std::array<ayt::stats::Value, 1> damage{60};
    const ActionCombatGroupHit2DFactory makeHit =
        [&](std::uint64_t sourceId, const ayt::ay2d::ActionHit2D& hit,
            const ayt::ay2d::ActionStep2D&) -> Result<CombatBatchHit> {
        SettlementRequest request;
        request.hit = {{1, sourceId}, sourceId, {1, hit.target, 1}, 0};
        request.source.instigator = CombatantRef{1, sourceId, 1};
        request.source.team = 10;
        request.target = {{1, hit.target, 1}, 20,
            mask(Capability::Damageable)};
        request.resource = ayt::stats::statId("health");
        request.expectedVersion = health.version();
        request.calculation = {.seed=123, .calculation=1000 + sourceId,
                               .hit=2000 + sourceId};
        return CombatBatchHit{request, &formula, damage};
    };
    const auto canHit = [](std::uint64_t sourceId, std::uint64_t targetId) {
        return sourceId < 3 && targetId == 3;
    };
    ayt::app::WorldStateLedger world;
    ayt::app::DurableOutbox outbox;
    ActionCombatGroup2DResult result;
    result.actions.resize(1);
    result.actions.front().clip = "unchanged";
    bool allowSave = false;
    int saves = 0;
    const Action2DWorldSave save = [&](const Action2DWorldCandidate& candidate) {
        ++saves;
        CHECK_TRUE(candidate.actorIds == std::vector<std::uint64_t>({1, 2, 3}));
        CHECK_TRUE(candidate.actions.size() == 3);
        CHECK_TRUE(candidate.actions[0].nextTick == 1
            && candidate.actions[2].nextTick == 1);
        CHECK_TRUE(candidate.stats[0].read(ayt::stats::statId("health"))->current == 0);
        CHECK_TRUE(candidate.life[0]->phase() == LifePhase::Dead);
        CHECK_TRUE(candidate.hits.attacks.size() == 2);
        CHECK_TRUE(candidate.outbox.pending().size() == 2);
        CHECK_TRUE(candidate.world.hasReceipt("action/tick-0"));
        CHECK_TRUE(candidate.result.combat.size() == 2);
        int deaths = 0;
        for (const auto& outcome : candidate.result.combat)
            if (outcome.life && outcome.life->after == LifePhase::Dead) ++deaths;
        CHECK_TRUE(deaths == 1);
        if (!allowSave) return false;
        std::string encoded, error;
        CHECK_TRUE(encodeAction2DWorldCandidate(candidate, encoded, &error));
        return static_cast<bool>(saveGame.save("combat", 1, encoded));
    };
    const auto commit = [&](std::string fingerprint,
        const ActionCombatPreflight& preflight = {}) {
        return commitActionGroupWorld2D("action/tick-0", std::move(fingerprint),
            0, actors, canHit, {}, {}, makeHit, targets, relations, hits,
            world, outbox, save, result, nullptr, preflight);
    };
    CHECK_TRUE(commit("payload-v1") == Action2DWorldCommitStatus::SaveRejected);
    CHECK_TRUE(first.state().nextTick == 0 && second.state().nextTick == 0
        && victim.state().nextTick == 0);
    CHECK_TRUE(health.read(ayt::stats::statId("health"))->current == 100);
    CHECK_TRUE(life.phase() == LifePhase::Alive && hits.retainedRequests() == 0);
    CHECK_TRUE(!world.hasReceipt("action/tick-0") && outbox.size() == 0);
    CHECK_TRUE(result.actions.size() == 1 && result.actions.front().clip == "unchanged");

    allowSave = true;
    const ActionCombatPreflight stale = [](std::span<const CombatBatchHit>)
        -> Result<void> { return std::unexpected(Diagnostic{
            Error::Stale, "Entity generation changed", {}}); };
    CHECK_TRUE(commit("payload-v1", stale) == Action2DWorldCommitStatus::Rejected);
    CHECK_TRUE(saves == 1 && health.read(ayt::stats::statId("health"))->current == 100);
    CHECK_TRUE(commit("payload-v1") == Action2DWorldCommitStatus::Committed);
    CHECK_TRUE(saves == 2 && first.state().nextTick == 1
        && second.state().nextTick == 1 && victim.state().nextTick == 1);
    CHECK_TRUE(health.read(ayt::stats::statId("health"))->current == 0);
    CHECK_TRUE(life.phase() == LifePhase::Dead && life.sequence() == 1);
    CHECK_TRUE(hits.retainedRequests() == 2 && world.hasReceipt("action/tick-0")
        && outbox.pending().size() == 2);
    CHECK_TRUE(commit("payload-v1") == Action2DWorldCommitStatus::AlreadyCommitted);
    CHECK_TRUE(commit("changed-payload") == Action2DWorldCommitStatus::Rejected);
    CHECK_TRUE(saves == 2 && hits.retainedRequests() == 2);

    const auto loaded = saveGame.load("combat", 1);
    CHECK_TRUE(static_cast<bool>(loaded));
    Action2DWorldSnapshot restored;
    const std::array<std::shared_ptr<const ayt::stats::Schema>, 1> schemas{schema};
    std::string error;
    CHECK_TRUE(decodeAction2DWorldSnapshot(loaded.payloadJson, schemas,
        restored, &error));
    CHECK_TRUE(restored.actorIds.size() == 3 && restored.targetIds.size() == 1);
    ayt::ay2d::ActionRuntime2D restoredFirst(clips);
    CHECK_TRUE(restoredFirst.restore(restored.actions[0]));
    CHECK_TRUE(restoredFirst.state().nextTick == 1);
    HitLedger restoredHits;
    CHECK_TRUE(restoredHits.restore(restored.hits));
    CHECK_TRUE(restoredHits.retainedRequests() == 2);
    CHECK_TRUE(restored.stats[0].read(ayt::stats::statId("health"))->current == 0);
    LifeTracker restoredLife({ayt::stats::statId("health"), LifePhase::Dead});
    CHECK_TRUE(restoredLife.restore(*restored.life[0]));
    CHECK_TRUE(restoredLife.phase() == LifePhase::Dead
        && restoredLife.sequence() == 1);
    CHECK_TRUE(restored.world.hasReceipt("action/tick-0")
        && restored.outbox.pending().size() == 2);
    Action2DWorldSnapshot unchanged;
    unchanged.actorIds = {77};
    CHECK_FALSE(decodeAction2DWorldSnapshot("{\"version\":1}", schemas,
        unchanged, &error));
    CHECK_TRUE(unchanged.actorIds == std::vector<std::uint64_t>{77});
    auto forged = nlohmann::json::parse(loaded.payloadJson);
    forged["actorIds"][0] = -1;
    CHECK_FALSE(decodeAction2DWorldSnapshot(forged.dump(), schemas,
        unchanged, &error));
    CHECK_TRUE(unchanged.actorIds == std::vector<std::uint64_t>{77});

    Action2DWorldCandidate savedAgain;
    savedAgain.actorIds = restored.actorIds;
    savedAgain.actions = restored.actions;
    savedAgain.targetIds = restored.targetIds;
    savedAgain.stats = restored.stats;
    savedAgain.hits = restored.hits;
    savedAgain.world = restored.world;
    savedAgain.outbox = restored.outbox;
    for (const auto& state : restored.life) {
        if (!state) savedAgain.life.emplace_back(std::nullopt);
        else {
            LifeTracker tracked(state->policy);
            CHECK_TRUE(tracked.restore(*state));
            savedAgain.life.emplace_back(tracked);
        }
    }
    std::set<std::string> received;
    int deliveryCalls = 0;
    const auto deliver = [&](const ayt::app::OutboxEntry& entry) {
        ++deliveryCalls;
        received.insert(entry.key);
        return true;
    };
    bool rejectAck = true;
    const auto saveAck = [&](const ayt::app::DurableOutbox& candidateOutbox) {
        if (rejectAck) return false;
        savedAgain.outbox = candidateOutbox;
        std::string encoded;
        if (!encodeAction2DWorldCandidate(savedAgain, encoded)) return false;
        return static_cast<bool>(saveGame.save("combat", 1, encoded));
    };
    const auto failedDelivery = ayt::app::dispatchPendingOutbox(
        restored.outbox, deliver, saveAck);
    CHECK_TRUE(failedDelivery.error == ayt::app::OutboxDispatchError::SaveRejected);
    CHECK_TRUE(deliveryCalls == 1 && restored.outbox.pending().size() == 2);
    rejectAck = false;
    const auto delivered = ayt::app::dispatchPendingOutbox(
        restored.outbox, deliver, saveAck);
    CHECK_TRUE(static_cast<bool>(delivered));
    CHECK_TRUE(delivered.acknowledged == 2 && restored.outbox.pending().empty());
    CHECK_TRUE(deliveryCalls == 3 && received.size() == 2);
    const auto afterDelivery = saveGame.load("combat", 1);
    Action2DWorldSnapshot acknowledged;
    CHECK_TRUE(static_cast<bool>(afterDelivery));
    CHECK_TRUE(decodeAction2DWorldSnapshot(afterDelivery.payloadJson,
        schemas, acknowledged));
    CHECK_TRUE(acknowledged.outbox.pending().empty()
        && acknowledged.world.hasReceipt("action/tick-0"));
}

TEST_SUITE_END
