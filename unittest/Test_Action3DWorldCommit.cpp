#include <AYTest.h>
#include <AYCombat/Action3DWorldCommit.h>
#include <AYApplication/SaveGameService.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace ayt::combat;

TEST_SUITE(Action3DWorldCommitTests)
TEST_CASE(previewed_two_window_hit_saves_with_pose_before_publication) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("ay-action3d-world-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
    ayt::app::SaveGameService saveGame({root.string(), "combat3d-world-test"});
    ayt::ability::FixedActionDefinition strike;
    strike.id = 1;
    strike.activeTicks = 2;
    strike.hitWindows = {{0, 1, 7}, {0, 1, 8}};
    auto compiled = ayt::ability::FixedActionTimeline::compile({strike});
    CHECK_TRUE(compiled.has_value());
    auto timeline = std::move(*compiled);
    CHECK_TRUE(timeline.start(1, 11, 1).has_value());
    const auto expected = timeline.checkpoint();
    CHECK_TRUE(timeline.previewStep(1)->hitWindows.size() == 2);
    const auto schema = ayt::stats::Schema::compile({{
        .name="health", .shape=ayt::stats::Shape::Pool,
        .current=100, .capacity=100}}).value();
    auto health = ayt::stats::StatSet::create(schema).value();
    LifeTracker life({ayt::stats::statId("health"), LifePhase::Dead});
    const CombatantRef target{1, 3, 1};
    const std::array<CombatBatchTarget, 1> targets{{{target, &health, &life}}};
    HitLedger hits;
    CHECK_TRUE(hits.open({1, 1}, {HitScope::Segment}, 0, 100).has_value());
    RelationTable relations;
    CHECK_TRUE(relations.set(10, 20, Relation::Hostile).has_value());
    const auto formula = ayt::stats::CalculationPlan::compile({{
        .id=1, .op=ayt::stats::Op::Input, .binding=0}}, 1, 1).value();
    const std::array<ayt::stats::Value, 1> damage{60};
    const ActionCombatHit3DFactory query = [&](const ayt::ability::FixedActionFrame& frame)
        -> Result<std::vector<CombatBatchHit>> {
        std::vector<CombatBatchHit> found;
        for (std::size_t i = 0; i < frame.hitWindows.size(); ++i) {
            SettlementRequest request;
            request.hit = {{1, 1}, i + 1, target, 1, 0,
                frame.hitWindows[i].id};
            request.source.instigator = CombatantRef{1, 1, 1};
            request.source.team = 10;
            request.source.ability = 1;
            request.source.abilityInstance = frame.execution;
            request.target = {target, 20, mask(Capability::Damageable)};
            request.resource = ayt::stats::statId("health");
            request.expectedVersion = health.version();
            request.calculation = {.seed=123, .tick=1,
                .calculation=100 + i, .hit=200 + i};
            found.push_back({request, &formula, damage});
        }
        return found;
    };
    ayt::app::WorldStateLedger world;
    ayt::app::DurableOutbox outbox;
    ActionCombatBatch3DResult result;
    result.frame.action = 99;
    bool allowSave = false;
    bool throwSave = false;
    int saves = 0;
    const Action3DWorldSave save = [&](const Action3DWorldCandidate& candidate) {
        ++saves;
        CHECK_TRUE(candidate.action.lastTick == 1);
        CHECK_TRUE(candidate.stats[0].read(ayt::stats::statId("health"))->current == 0);
        CHECK_TRUE(candidate.life[0]->phase() == LifePhase::Dead);
        CHECK_TRUE(candidate.result.combat.size() == 2);
        CHECK_TRUE(candidate.outbox.pending().size() == 2);
        CHECK_TRUE(candidate.world.hasReceipt("action3d/tick-1"));
        if (throwSave) throw std::runtime_error("injected save failure");
        if (!allowSave) return false;
        std::string encoded;
        CHECK_TRUE(encodeAction3DWorldCandidate(candidate, encoded));
        nlohmann::json slot{{"combat", encoded}, {"physicsPoseX", 1.5}};
        return static_cast<bool>(saveGame.save("combat3d", 1, slot.dump()));
    };
    const auto commit = [&](std::string fingerprint,
        const ActionCombatPreflight& preflight = {}) {
        return commitActionWorld3D("action3d/tick-1", std::move(fingerprint),
            timeline, 1, expected, query, targets, relations, hits, world,
            outbox, save, result, nullptr, preflight);
    };
    CHECK_TRUE(commit("payload-v1") == Action3DWorldCommitStatus::SaveRejected);
    CHECK_TRUE(timeline.checkpoint() == expected
        && health.read(ayt::stats::statId("health"))->current == 100
        && life.phase() == LifePhase::Alive && hits.retainedRequests() == 0);
    CHECK_TRUE(!world.hasReceipt("action3d/tick-1") && outbox.size() == 0
        && result.frame.action == 99);
    throwSave = true;
    CHECK_TRUE(commit("payload-v1") == Action3DWorldCommitStatus::SaveRejected);
    CHECK_TRUE(timeline.checkpoint() == expected
        && health.read(ayt::stats::statId("health"))->current == 100);
    throwSave = false;
    auto wrongExpected = expected;
    ++wrongExpected.lastTick;
    CHECK_TRUE(commitActionWorld3D("action3d/tick-1", "payload-v1",
        timeline, 1, wrongExpected, query, targets, relations, hits, world,
        outbox, save, result) == Action3DWorldCommitStatus::Rejected);
    allowSave = true;
    const ActionCombatPreflight stale = [](std::span<const CombatBatchHit>)
        -> Result<void> { return std::unexpected(Diagnostic{
            Error::Stale, "target generation changed", {}}); };
    CHECK_TRUE(commit("payload-v1", stale) == Action3DWorldCommitStatus::Rejected);
    CHECK_TRUE(saves == 2 && timeline.checkpoint() == expected);
    CHECK_TRUE(commit("payload-v1") == Action3DWorldCommitStatus::Committed);
    CHECK_TRUE(saves == 3 && timeline.checkpoint().lastTick == 1
        && result.frame.execution == 11 && result.combat.size() == 2);
    CHECK_TRUE(health.read(ayt::stats::statId("health"))->current == 0
        && life.phase() == LifePhase::Dead && life.sequence() == 1);
    CHECK_TRUE(hits.retainedRequests() == 2 && outbox.pending().size() == 2);
    CHECK_TRUE(commit("payload-v1") == Action3DWorldCommitStatus::AlreadyCommitted);
    CHECK_TRUE(commit("changed") == Action3DWorldCommitStatus::Rejected);
    CHECK_TRUE(saves == 3 && hits.retainedRequests() == 2);

    const auto loaded = saveGame.load("combat3d", 1);
    CHECK_TRUE(static_cast<bool>(loaded));
    const auto slot = nlohmann::json::parse(loaded.payloadJson);
    CHECK_TRUE(slot.at("physicsPoseX") == 1.5);
    const std::array<std::shared_ptr<const ayt::stats::Schema>, 1> schemas{schema};
    Action3DWorldSnapshot restored;
    CHECK_TRUE(decodeAction3DWorldSnapshot(slot.at("combat").get<std::string>(),
        schemas, restored));
    CHECK_TRUE(restored.action.lastTick == 1 && restored.targetIds[0] == target);
    CHECK_TRUE(restored.stats[0].read(ayt::stats::statId("health"))->current == 0);
    CHECK_TRUE(restored.life[0]->phase == LifePhase::Dead
        && restored.life[0]->sequence == 1);
    CHECK_TRUE(restored.world.hasReceipt("action3d/tick-1")
        && restored.outbox.pending().size() == 2);
    auto restoredTimeline = ayt::ability::FixedActionTimeline::compile({strike}).value();
    CHECK_TRUE(restoredTimeline.restore(restored.action).has_value());
    HitLedger restoredHits;
    CHECK_TRUE(restoredHits.restore(restored.hits)
        && restoredHits.retainedRequests() == 2);
    Action3DWorldSnapshot unchanged;
    unchanged.action.execution = 77;
    CHECK_FALSE(decodeAction3DWorldSnapshot("{\"version\":1}", schemas,
        unchanged));
    CHECK_TRUE(unchanged.action.execution == 77);
    auto forged = nlohmann::json::parse(slot.at("combat").get<std::string>());
    forged["action"][8] = 99;
    CHECK_FALSE(decodeAction3DWorldSnapshot(forged.dump(), schemas, unchanged));
    CHECK_TRUE(unchanged.action.execution == 77);
}
TEST_SUITE_END
