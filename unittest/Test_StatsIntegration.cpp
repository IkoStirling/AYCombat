#include <AYTest.h>
#include <AYCombat/StatsIntegration.h>
#include "Fixtures.h"
#include <array>
#ifdef AY_COMBAT_WITH_AY2D
#include <AYCombat/Action2DBatch.h>
#endif

namespace {
ayt::stats::StatSet stats() {
    auto schema = ayt::stats::Schema::compile({ayt::stats::Definition{
        .name = "health", .shape = ayt::stats::Shape::Pool, .current = 100, .capacity = 100}}).value();
    return ayt::stats::StatSet::create(schema).value();
}
ayt::stats::StatSet armoredStats() {
    auto schema = ayt::stats::Schema::compile({
        ayt::stats::Definition{.name = "armor", .shape = ayt::stats::Shape::Pool,
            .current = 50, .capacity = 50},
        ayt::stats::Definition{.name = "health", .shape = ayt::stats::Shape::Pool,
            .current = 100, .capacity = 100}}).value();
    return ayt::stats::StatSet::create(schema).value();
}
ayt::stats::CalculationPlan plan() {
    return ayt::stats::CalculationPlan::compile({{.id = 1, .op = ayt::stats::Op::Input, .binding = 0}}, 1, 1).value();
}
SettlementRequest request(std::uint64_t id, const ayt::stats::StatSet& state) {
    return {hit(id), source(), target(), {}, ayt::stats::statId("health"), state.version(),
            {.seed = 123, .calculation = 1000 + id, .hit = 2000 + id}};
}
}

TEST_SUITE(CombatStatsIntegrationTests)

TEST_CASE(life_tracker_snapshot_restores_sequence_without_changing_policy) {
    LifeTracker life({ayt::stats::statId("health"), LifePhase::Dead});
    CHECK_TRUE(life.observeCurrent(10, 0).has_value());
    const auto saved = life.snapshot();
    CHECK_TRUE(saved.phase == LifePhase::Dead && saved.sequence == 1);
    LifeTracker restored(saved.policy);
    CHECK_TRUE(restored.restore(saved));
    CHECK_TRUE(restored.phase() == LifePhase::Dead && restored.sequence() == 1);
    auto invalid = saved;
    invalid.policy.healingRevives = true;
    CHECK_FALSE(restored.restore(invalid));
    CHECK_TRUE(restored.sequence() == 1);
}

TEST_CASE(batch_failure_keeps_all_target_stats_life_receipts_and_output) {
    auto first = stats(), second = stats();
    auto book = ledger();
    auto table = relations();
    const auto formula = plan();
    LifeTracker life({ayt::stats::statId("health"), LifePhase::Downed});
    const std::array<ayt::stats::Value, 1> fatal{100}, minor{20};
    auto a = request(1, first);
    auto b = request(2, second);
    b.hit.target = actor(3);
    b.target.ref = actor(3);
    b.resource = ayt::stats::statId("missing");
    std::array<CombatBatchHit, 2> hits{{{a, &formula, fatal},
                                         {b, &formula, minor}}};
    const std::array<CombatBatchTarget, 2> targets{{
        {actor(2), &first, &life}, {actor(3), &second, nullptr}}};
    std::vector<CombatOutcome> outcomes(1);
    const auto initialVersion = first.version();
    const std::array<CombatBatchTarget, 2> aliased{{
        {actor(2), &first, &life}, {actor(3), &first, nullptr}}};
    CHECK_TRUE(settleBatch(hits, aliased, table, book, outcomes).error().code
        == Error::Duplicate);
    auto splitTicks = hits;
    splitTicks[1].request.hit.tick = hits[0].request.hit.tick + 1;
    CHECK_TRUE(settleBatch(splitTicks, targets, table, book, outcomes)
        .error().code == Error::Invalid);
    CHECK_TRUE(settleBatch(hits, targets, table, book, outcomes).error().code
        == Error::Missing);
    CHECK_TRUE(first.version() == initialVersion
        && second.version() == initialVersion
        && first.read(a.resource)->current == 100
        && second.read(a.resource)->current == 100
        && life.phase() == LifePhase::Alive
        && book.retainedRequests() == 0
        && outcomes.size() == 1);

    hits[1].request.resource = a.resource;
    CHECK_TRUE(settleBatch(hits, targets, table, book, outcomes).has_value());
    CHECK_TRUE(first.read(a.resource)->current == 0
        && second.read(a.resource)->current == 80
        && life.phase() == LifePhase::Downed
        && book.retainedRequests() == 2
        && outcomes.size() == 2
        && outcomes[0].life && outcomes[0].life->after == LifePhase::Downed);
    hits[0].request.expectedVersion = first.version();
    hits[1].request.expectedVersion = second.version();
    CHECK_TRUE(settleBatch(hits, targets, table, book, outcomes).error().code
        == Error::Duplicate);
    CHECK_TRUE(first.read(a.resource)->current == 0
        && second.read(a.resource)->current == 80
        && outcomes.size() == 2);
}

TEST_CASE(batch_rebases_same_target_hits_on_staged_versions_in_caller_order) {
    auto state = stats();
    auto book = ledger({HitScope::Segment});
    const auto formula = plan();
    const std::array<ayt::stats::Value, 1> amount{80};
    auto a = request(1, state);
    auto b = request(2, state);
    b.hit.segment = 1;
    const std::array<CombatBatchHit, 2> hits{{{a, &formula, amount},
                                               {b, &formula, amount}}};
    const std::array<CombatBatchTarget, 1> targets{{{actor(2), &state, nullptr}}};
    std::vector<CombatOutcome> outcomes;
    CHECK_TRUE(settleBatch(hits, targets, relations(), book, outcomes).has_value());
    CHECK_TRUE(state.read(a.resource)->current == 0
        && outcomes.size() == 2
        && outcomes[0].settlement.changes[0].actual == -80
        && outcomes[1].settlement.changes[0].actual == -20
        && outcomes[1].settlement.changes[0].excess == 60);
}

#ifdef AY_COMBAT_WITH_AY2D
TEST_CASE(action_path_and_combat_batch_publish_together_or_not_at_all) {
    ayt::ay2d::ActionFrame2D strike;
    strike.hitBoxes = {{0.0f, 0.0f, 1.0f, 1.0f}};
    ayt::ay2d::ActionLibrary2D clips{{{"strike", {strike}, true}}};
    ayt::ay2d::ActionRuntime2D action(clips);
    const std::vector<ayt::ay2d::ActionPathPoint2D> path{
        {0.0f, {0.0f, 0.0f}}, {1.0f, {10.0f, 0.0f}}};
    const std::vector<ayt::ay2d::ActionTarget2D> actionTargets{
        {11, {{3.0f, 0.0f, 4.0f, 1.0f}}, true},
        {12, {{7.0f, 0.0f, 8.0f, 1.0f}}, true}};
    auto first = stats(), second = stats();
    const std::array<CombatBatchTarget, 2> combatTargets{{
        {actor(2), &first, nullptr}, {actor(3), &second, nullptr}}};
    auto book = ledger();
    const auto table = relations();
    const auto formula = plan();
    const std::array<ayt::stats::Value, 1> amount{20};
    bool invalidSecond = true;
    const ActionCombatHit2DFactory makeHit = [&](const ayt::ay2d::ActionHit2D& hit,
        const ayt::ay2d::ActionStep2D&) -> Result<CombatBatchHit> {
        auto req = request(hit.target == 11 ? 1 : 2,
                           hit.target == 11 ? first : second);
        if (hit.target == 12) {
            req.hit.target = actor(3);
            req.target.ref = actor(3);
            if (invalidSecond) req.resource = ayt::stats::statId("missing");
        }
        return CombatBatchHit{req, &formula, amount};
    };
    ActionCombatBatch2DResult output;
    output.action.clip = "unchanged";
    CHECK_TRUE(!stepAndSettleActionPath2D(action, 0, {"strike", true},
        path, actionTargets, {}, makeHit, combatTargets, table, book,
        output).has_value());
    CHECK_TRUE(action.state().nextTick == 0
        && first.read(ayt::stats::statId("health"))->current == 100
        && second.read(ayt::stats::statId("health"))->current == 100
        && book.retainedRequests() == 0
        && output.action.clip == "unchanged"
        && output.combat.empty());

    const ActionCombatHit2DFactory wrongTick = [&](const ayt::ay2d::ActionHit2D& hit,
        const ayt::ay2d::ActionStep2D& step) -> Result<CombatBatchHit> {
        auto bound = makeHit(hit, step);
        bound->request.hit.tick = 1;
        return bound;
    };
    CHECK_TRUE(!stepAndSettleActionPath2D(action, 0, {"strike", true},
        path, actionTargets, {}, wrongTick, combatTargets, table, book,
        output).has_value());
    CHECK_TRUE(action.state().nextTick == 0
        && book.retainedRequests() == 0
        && output.action.clip == "unchanged");

    invalidSecond = false;
    const ActionCombatPreflight staleWorld = [](std::span<const CombatBatchHit>)
        -> Result<void> {
        return std::unexpected(Diagnostic{Error::Stale,
            "target generation changed", {}});
    };
    CHECK_TRUE(stepAndSettleActionPath2D(action, 0, {"strike", true},
        path, actionTargets, {}, makeHit, combatTargets, table, book,
        output, nullptr, staleWorld).error().code == Error::Stale);
    CHECK_TRUE(action.state().nextTick == 0
        && first.read(ayt::stats::statId("health"))->current == 100
        && second.read(ayt::stats::statId("health"))->current == 100
        && book.retainedRequests() == 0);
    CHECK_TRUE(stepAndSettleActionPath2D(action, 0, {"strike", true},
        path, actionTargets, {}, makeHit, combatTargets, table, book,
        output).has_value());
    CHECK_TRUE(action.state().nextTick == 1
        && output.action.hits.size() == 2
        && output.combat.size() == 2
        && first.read(ayt::stats::statId("health"))->current == 80
        && second.read(ayt::stats::statId("health"))->current == 80
        && book.retainedRequests() == 2);
}

TEST_CASE(action_path_batch_without_hit_advances_only_action) {
    ayt::ay2d::ActionFrame2D strike;
    strike.hitBoxes = {{0.0f, 0.0f, 1.0f, 1.0f}};
    ayt::ay2d::ActionLibrary2D clips{{{"strike", {strike}, true}}};
    ayt::ay2d::ActionRuntime2D action(clips);
    auto book = ledger();
    ActionCombatBatch2DResult output;
    CHECK_TRUE(stepAndSettleActionPath2D(action, 0, {"strike", true},
        {{0.0f, {0.0f, 0.0f}}, {1.0f, {0.0f, 0.0f}}},
        {}, {}, {}, {}, relations(), book, output).has_value());
    CHECK_TRUE(action.state().nextTick == 1
        && output.action.active && output.action.hits.empty()
        && output.combat.empty() && book.retainedRequests() == 0);
}

TEST_CASE(moving_blocker_culls_late_action_combat_hit_in_one_batch) {
    ayt::ay2d::ActionFrame2D strike;
    strike.hitBoxes = {{0.0f, 0.0f, 1.0f, 1.0f}};
    ayt::ay2d::ActionLibrary2D clips{{{"strike", {strike}, false}}};
    ayt::ay2d::ActionRuntime2D action(clips);
    const std::vector<ayt::ay2d::ActionPathPoint2D> path{
        {0.0f, {0.0f, 0.0f}}, {1.0f, {10.0f, 0.0f}}};
    const std::vector<ayt::ay2d::ActionTarget2D> actionTargets{
        {2, {{3.0f, 0.0f, 4.0f, 1.0f}}, true},
        {3, {{8.0f, 0.0f, 9.0f, 1.0f}}, true}};
    const std::array<ayt::ay2d::ActionMovingBlocker2D, 1> closing{{
        {{20.0f, 0.0f, 20.5f, 1.0f},
         {{0.0f, {20.0f, 0.0f}}, {0.5f, {5.0f, 0.0f}},
          {1.0f, {20.0f, 0.0f}}}}}};
    auto nearHealth = stats(), farHealth = stats();
    const std::array<CombatBatchTarget, 2> targets{{
        {actor(2), &nearHealth, nullptr}, {actor(3), &farHealth, nullptr}}};
    auto book = ledger();
    const auto formula = plan();
    const std::array<ayt::stats::Value, 1> amount{20};
    const ActionCombatHit2DFactory makeHit = [&](const ayt::ay2d::ActionHit2D& hit,
        const ayt::ay2d::ActionStep2D&) -> Result<CombatBatchHit> {
        auto& health = hit.target == 2 ? nearHealth : farHealth;
        auto req = request(hit.target, health);
        req.hit.target = actor(hit.target);
        req.target.ref = actor(hit.target);
        return CombatBatchHit{req, &formula, amount};
    };
    ActionCombatBatch2DResult output;
    CHECK_TRUE(stepAndSettleActionPath2D(action, 0, {"strike", true},
        path, actionTargets, {}, closing, makeHit, targets, relations(),
        book, output).has_value());
    CHECK_TRUE(action.state().nextTick == 1 && output.action.hits.size() == 1
        && output.action.hits[0].target == 2 && output.combat.size() == 1
        && nearHealth.read(ayt::stats::statId("health"))->current == 80
        && farHealth.read(ayt::stats::statId("health"))->current == 100
        && book.retainedRequests() == 1);
}

TEST_CASE(authored_action_windows_settle_as_distinct_combat_segments) {
    ayt::ay2d::ActionFrame2D first;
    first.hitBoxes = {{0.0f, 0.0f, 1.0f, 1.0f}};
    auto second = first;
    second.resetHitsOnEnter = true;
    ayt::ay2d::ActionLibrary2D clips{{{"double", {first, second}, false}}};
    ayt::ay2d::ActionRuntime2D action(clips);
    auto health = stats();
    auto book = ledger({HitScope::Segment});
    const auto formula = plan();
    const std::array<ayt::stats::Value, 1> amount{20};
    const std::array<CombatBatchTarget, 1> combatTargets{{
        {actor(2), &health, nullptr}}};
    const std::vector<ayt::ay2d::ActionPathPoint2D> path{
        {0.0f, {0.0f, 0.0f}}, {1.0f, {0.0f, 0.0f}}};
    const std::vector<ayt::ay2d::ActionTarget2D> targets{
        {2, {{0.25f, 0.25f, 0.75f, 0.75f}}, true}};
    std::uint64_t tick = 0;
    const ActionCombatHit2DFactory makeHit = [&](const ayt::ay2d::ActionHit2D& hit,
        const ayt::ay2d::ActionStep2D& step) -> Result<CombatBatchHit> {
        auto req = request(tick + 1, health);
        req.hit.target = actor(hit.target);
        req.hit.tick = tick;
        req.hit.segment = step.hitWindow;
        req.calculation.tick = tick;
        return CombatBatchHit{req, &formula, amount};
    };
    ActionCombatBatch2DResult output;
    CHECK_TRUE(stepAndSettleActionPath2D(action, tick, {"double", true},
        path, targets, {}, makeHit, combatTargets, relations(), book,
        output).has_value());
    CHECK_TRUE(output.action.hitWindow == 0 && output.action.hits.size() == 1
        && health.read(ayt::stats::statId("health"))->current == 80);
    tick = 1;
    CHECK_TRUE(stepAndSettleActionPath2D(action, tick, {}, path, targets, {},
        makeHit, combatTargets, relations(), book, output).has_value());
    CHECK_TRUE(output.action.hitWindow == 1 && output.action.hits.size() == 1
        && health.read(ayt::stats::statId("health"))->current == 60
        && book.retainedRequests() == 2);
}

TEST_CASE(action_group_and_combat_batch_reject_second_hit_without_partial_trade) {
    ayt::ay2d::ActionFrame2D strike;
    strike.hitBoxes = {{0.2f, -0.2f, 1.0f, 0.2f}};
    strike.hurtBoxes = {{-0.2f, -0.5f, 0.2f, 0.5f}};
    ayt::ay2d::ActionLibrary2D clips{{{"strike", {strike}, true}}};
    ayt::ay2d::ActionRuntime2D left(clips), right(clips);
    const std::vector<ayt::ay2d::ActionActor2D> actors{
        {1, &left, {"strike", true}, 0.0f, 0.0f},
        {2, &right, {"strike", false}, 0.6f, 0.0f}};
    auto leftHealth = stats(), rightHealth = stats();
    const std::array<CombatBatchTarget, 2> targets{{
        {actor(1), &leftHealth, nullptr},
        {actor(2), &rightHealth, nullptr}}};
    auto book = ledger();
    CHECK_TRUE(book.open({1, 2}, {}, 0, 100).has_value());
    auto table = relations();
    CHECK_TRUE(table.set(20, 10, Relation::Hostile).has_value());
    const auto formula = plan();
    const std::array<ayt::stats::Value, 1> amount{20};
    bool invalidSecond = true;
    const ActionCombatGroupHit2DFactory makeHit =
        [&](std::uint64_t sourceId, const ayt::ay2d::ActionHit2D& actionHit,
            const ayt::ay2d::ActionStep2D&) -> Result<CombatBatchHit> {
        const auto& targetState = actionHit.target == 1 ? leftHealth : rightHealth;
        auto req = request(100 + sourceId, targetState);
        req.hit.attack = {1, sourceId};
        req.hit.target = actor(actionHit.target);
        req.source.instigator = actor(sourceId);
        req.source.team = sourceId == 1 ? 10 : 20;
        req.target.ref = actor(actionHit.target);
        req.target.team = sourceId == 1 ? 20 : 10;
        if (sourceId == 2 && invalidSecond)
            req.resource = ayt::stats::statId("missing");
        return CombatBatchHit{req, &formula, amount};
    };
    ActionCombatGroup2DResult output;
    output.actions.resize(1);
    output.actions.front().clip = "unchanged";
    CHECK_TRUE(!stepAndSettleActionGroup2D(0, actors, {}, {}, makeHit,
        targets, table, book, output).has_value());
    CHECK_TRUE(left.state().nextTick == 0 && right.state().nextTick == 0
        && leftHealth.read(ayt::stats::statId("health"))->current == 100
        && rightHealth.read(ayt::stats::statId("health"))->current == 100
        && book.retainedRequests() == 0
        && output.actions.size() == 1
        && output.actions.front().clip == "unchanged"
        && output.combat.empty());

    invalidSecond = false;
    const std::array<ayt::ay2d::ActionMovingBlocker2D, 1> farBlocker{{
        {{10.0f, 10.0f, 11.0f, 11.0f},
         {{0.0f, {10.0f, 10.0f}}, {1.0f, {10.0f, 10.0f}}}}}};
    CHECK_TRUE(stepAndSettleActionGroup2D(0, actors, {}, {}, farBlocker, makeHit,
        targets, table, book, output).has_value());
    CHECK_TRUE(left.state().nextTick == 1 && right.state().nextTick == 1
        && output.actions.size() == 2 && output.combat.size() == 2
        && output.actions[0].hits.size() == 1
        && output.actions[1].hits.size() == 1
        && leftHealth.read(ayt::stats::statId("health"))->current == 80
        && rightHealth.read(ayt::stats::statId("health"))->current == 80
        && book.retainedRequests() == 2);
}
#endif

TEST_CASE(one_hit_commits_armor_and_health_together) {
    auto state = armoredStats(); auto book = ledger(); auto r = request(1, state);
    const std::array<ayt::stats::Mutation, 2> changes{{
        {ayt::stats::statId("armor"), ayt::stats::MutationOp::Decrease, 30},
        {ayt::stats::statId("health"), ayt::stats::MutationOp::Decrease, 20}}};
    const auto result = settleResources(r, relations(), book, state, changes).value();
    CHECK_TRUE(result.state == OutcomeState::Applied && result.kind == OutcomeKind::Damage);
    CHECK_TRUE(result.calculated == 20 && result.settlement.changes.size() == 2);
    CHECK_TRUE(state.read(ayt::stats::statId("armor"))->current == 20
        && state.read(ayt::stats::statId("health"))->current == 80);
    r.expectedVersion = state.version();
    CHECK_TRUE(settleResources(r, relations(), book, state, changes).error().code
        == Error::Duplicate);
}

TEST_CASE(part_armor_route_absorbs_then_exposes_health_without_replay) {
    auto state = armoredStats(); auto book = ledger(); auto r = request(1, state);
    r.hit.part = 7;
    const PartArmorRoute route{7, ayt::stats::statId("health"),
        ayt::stats::statId("armor"), 7500};
    const auto preview = planPartArmorHit(r, state, route, 80).value();
    CHECK_TRUE(preview.absorbed == 50 && preview.healthDamage == 30
        && preview.mutationCount == 2);
    CHECK_TRUE(state.read(route.armor)->current == 50
        && state.read(route.health)->current == 100
        && book.retainedRequests() == 0);
    const auto outcome = settlePartArmorHit(r, relations(), book, state,
        route, 80).value();
    CHECK_TRUE(outcome.state == OutcomeState::Applied && outcome.part == 7
        && outcome.calculated == 30 && outcome.settlement.changes.size() == 2);
    CHECK_TRUE(state.read(route.armor)->current == 0
        && state.read(route.health)->current == 70);
    r.expectedVersion = state.version();
    CHECK_TRUE(settlePartArmorHit(r, relations(), book, state, route, 80)
        .error().code == Error::Duplicate);
    CHECK_TRUE(state.read(route.health)->current == 70);
}

TEST_CASE(part_armor_route_rejects_stale_and_invalid_without_reservation) {
    auto state = armoredStats(); auto book = ledger(); auto r = request(1, state);
    r.hit.part = 2;
    PartArmorRoute route{2, ayt::stats::statId("health"),
        ayt::stats::statId("armor"), 5000};
    const auto version = state.version();
    CHECK_TRUE(settlePartArmorHit(r, relations(), book, state,
        route, -1).error().code == Error::Invalid);
    route.part = 3;
    CHECK_TRUE(settlePartArmorHit(r, relations(), book, state,
        route, 30).error().code == Error::Invalid);
    route.part = 2; route.absorptionBasisPoints = 10001;
    CHECK_TRUE(settlePartArmorHit(r, relations(), book, state,
        route, 30).error().code == Error::Invalid);
    route.absorptionBasisPoints = 5000;
    r.expectedVersion = version + 1;
    CHECK_TRUE(settlePartArmorHit(r, relations(), book, state,
        route, 30).error().code == Error::Stale);
    CHECK_TRUE(state.version() == version && book.retainedRequests() == 0
        && state.read(route.armor)->current == 50);
    r.expectedVersion = version;
    CHECK_TRUE(settlePartArmorHit(r, relations(), book, state,
        route, 30).has_value());
    CHECK_TRUE(state.read(route.armor)->current == 35
        && state.read(route.health)->current == 85);
}

TEST_CASE(unarmored_part_routes_all_damage_to_one_health_pool) {
    auto state = armoredStats(); auto book = ledger(); auto r = request(1, state);
    r.hit.part = 4;
    const PartArmorRoute route{4, ayt::stats::statId("health"), 0, 0};
    const auto planned = planPartArmorHit(r, state, route, 13).value();
    CHECK_TRUE(planned.mutationCount == 1 && planned.absorbed == 0
        && planned.healthDamage == 13);
    LifeTracker life({route.health, LifePhase::Dead});
    const auto outcome = settlePartArmorHit(r, relations(), book, state,
        route, 13, {.life=&life}).value();
    CHECK_TRUE(outcome.kind == OutcomeKind::Damage
        && outcome.settlement.changes.size() == 1
        && state.read(route.health)->current == 87
        && state.read(ayt::stats::statId("armor"))->current == 50
        && life.phase() == LifePhase::Alive);
}

TEST_CASE(invalid_second_pool_keeps_first_pool_and_hit_retryable) {
    auto state = armoredStats(); auto book = ledger(); const auto r = request(1, state);
    const std::array<ayt::stats::Mutation, 2> invalid{{
        {ayt::stats::statId("armor"), ayt::stats::MutationOp::Decrease, 30},
        {ayt::stats::statId("missing"), ayt::stats::MutationOp::Decrease, 20}}};
    const auto version = state.version();
    CHECK_TRUE(settleResources(r, relations(), book, state, invalid).error().code
        == Error::Missing);
    CHECK_TRUE(state.version() == version && book.retainedRequests() == 0
        && state.read(ayt::stats::statId("armor"))->current == 50);
    const std::array<ayt::stats::Mutation, 2> valid{{
        invalid[0], {ayt::stats::statId("health"),
                     ayt::stats::MutationOp::Decrease, 20}}};
    CHECK_TRUE(settleResources(r, relations(), book, state, valid).has_value());
    CHECK_TRUE(state.read(ayt::stats::statId("health"))->current == 80);
}

TEST_CASE(multi_pool_life_transition_uses_final_health) {
    auto state = armoredStats(); auto book = ledger(); const auto r = request(1, state);
    LifeTracker life({ayt::stats::statId("health"), LifePhase::Downed});
    const std::array<ayt::stats::Mutation, 2> changes{{
        {ayt::stats::statId("armor"), ayt::stats::MutationOp::Decrease, 50},
        {ayt::stats::statId("health"), ayt::stats::MutationOp::Decrease, 100}}};
    const auto result = settleResources(r, relations(), book, state, changes,
                                        {.life=&life}).value();
    CHECK_TRUE(result.life && result.life->after == LifePhase::Downed);
    CHECK_TRUE(life.phase() == LifePhase::Downed
        && state.read(ayt::stats::statId("health"))->current == 0);
}

TEST_CASE(ordered_80_80_preserves_actual_80_20_and_excess) {
    auto state = stats(); auto book = ledger({HitScope::Segment}); auto table = relations(); const auto formula = plan();
    const std::array<ayt::stats::Value, 1> inputs{80};
    auto first = request(1, state);
    const auto a = settle(first, table, book, state, formula, inputs).value();
    auto second = request(2, state); second.hit.segment = 1;
    const auto b = settle(second, table, book, state, formula, inputs).value();
    CHECK_TRUE(a.state == OutcomeState::Applied && b.state == OutcomeState::Applied);
    CHECK_TRUE(a.settlement.changes[0].actual == -80 && b.settlement.changes[0].actual == -20);
    CHECK_TRUE(b.settlement.changes[0].excess == 60);
    CHECK_TRUE(state.read(first.resource).value().current == 0);
    CHECK_TRUE(a.source.attribution == 100 && b.source.effect == 0);
}

TEST_CASE(successful_replay_never_applies_a_second_resource_change) {
    auto state = stats(); auto book = ledger(); auto r = request(1, state);
    const std::array<ayt::stats::Value, 1> inputs{30};
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).has_value());
    r.expectedVersion = state.version();
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).error().code == Error::Duplicate);
    CHECK_TRUE(state.read(r.resource).value().current == 70);
}

TEST_CASE(stale_version_releases_allowance_and_same_identity_can_retry) {
    auto state = stats(); auto book = ledger(); auto r = request(1, state);
    --r.expectedVersion;
    const std::array<ayt::stats::Value, 1> inputs{30};
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).error().code == Error::Stale);
    CHECK_TRUE(state.read(r.resource).value().current == 100);
    r.expectedVersion = state.version();
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).has_value());
    CHECK_TRUE(state.read(r.resource).value().current == 70 && book.retainedRequests() == 1);
}

TEST_CASE(numerical_failure_does_not_consume_hit_or_modify_resources) {
    auto state = stats(); auto book = ledger(); auto r = request(1, state);
    const auto broken = ayt::stats::CalculationPlan::compile({
        {.id = 1, .op = ayt::stats::Op::Constant, .value = 10},
        {.id = 2, .op = ayt::stats::Op::Constant, .value = 0},
        {.id = 3, .op = ayt::stats::Op::Divide, .a = 1, .b = 2}}, 3, 0).value();
    const auto version = state.version();
    CHECK_TRUE(settle(r, relations(), book, state, broken, {}).error().code == Error::NumericFailure);
    CHECK_TRUE(state.version() == version);
    const std::array<ayt::stats::Value, 1> inputs{10};
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).has_value());
}

TEST_CASE(negative_damage_is_invalid_and_does_not_turn_into_healing) {
    auto state = stats(); auto book = ledger(); const auto r = request(1, state);
    const std::array<ayt::stats::Value, 1> inputs{-20};
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).error().code == Error::Invalid);
    CHECK_TRUE(state.read(r.resource).value().current == 100);
    CHECK_TRUE(book.close({1, 1}).has_value());
}

TEST_CASE(relation_change_rejects_without_reserving_hit_or_mutating_stats) {
    auto state = stats(); auto book = ledger(); const auto r = request(1, state); auto table = relations();
    const std::array<ayt::stats::Value, 1> inputs{30};
    CHECK_TRUE(table.set(10, 20, Relation::Friendly).has_value());
    const auto rejected = settle(r, table, book, state, plan(), inputs).value();
    CHECK_TRUE(rejected.state == OutcomeState::Rejected && rejected.decision.denial == Denial::RelationForbidden);
    CHECK_TRUE(book.retainedRequests() == 0 && state.read(r.resource).value().current == 100);
    CHECK_TRUE(table.set(10, 20, Relation::Hostile).has_value());
    CHECK_TRUE(settle(r, table, book, state, plan(), inputs).has_value());
}

TEST_CASE(preview_does_not_consume_hit_or_write_state) {
    auto state = stats(); auto book = ledger(); const auto r = request(1, state);
    const std::array<ayt::stats::Value, 1> inputs{25}; const auto version = state.version();
    const auto estimate = preview(r, relations(), plan(), inputs).value();
    CHECK_TRUE(estimate.decision.allowed && estimate.calculated == 25);
    CHECK_TRUE(state.version() == version && book.retainedRequests() == 0);
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).has_value());
}

TEST_CASE(healing_preserves_actual_restoration_and_overheal) {
    auto state = stats(); auto book = ledger({HitScope::Segment});
    auto r = request(1, state); const std::array<ayt::stats::Value, 1> damage{30};
    CHECK_TRUE(settle(r, relations(), book, state, plan(), damage).has_value());
    r = request(2, state); r.hit.segment = 1; r.filter.operation = Operation::Heal;
    const std::array<ayt::stats::Value, 1> healing{50};
    const auto healed = settle(r, relations(), book, state, plan(), healing).value();
    CHECK_TRUE(healed.operation == Operation::Heal && healed.settlement.changes[0].actual == 30);
    CHECK_TRUE(healed.settlement.changes[0].excess == 20 && state.read(r.resource).value().current == 100);
}

TEST_CASE(zero_damage_is_an_applied_hit_with_zero_actual_change) {
    auto state = stats(); auto book = ledger(); const auto r = request(1, state);
    const std::array<ayt::stats::Value, 1> inputs{0};
    const auto outcome = settle(r, relations(), book, state, plan(), inputs).value();
    CHECK_TRUE(outcome.state == OutcomeState::Applied && outcome.calculated == 0);
    CHECK_TRUE(outcome.settlement.changes[0].actual == 0);
    CHECK_TRUE(book.reserve(hit(2)).error().code == Error::AlreadyHit);
}

TEST_CASE(capacity_rejection_happens_before_numerical_commit) {
    auto state = stats(); HitLedger book({1, 1, 0});
    CHECK_TRUE(book.open({1, 1}, {}, 0, 100).has_value());
    const auto r = request(1, state); const auto version = state.version();
    const std::array<ayt::stats::Value, 1> inputs{30};
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).error().code == Error::Limit);
    CHECK_TRUE(state.version() == version && state.read(r.resource).value().current == 100);
}

TEST_CASE(invalid_binding_and_missing_resource_never_modify_target) {
    auto state = stats(); auto book = ledger(); auto r = request(1, state);
    const std::array<ayt::stats::Value, 1> inputs{30};
    r.target.ref = actor(3);
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).error().code == Error::Invalid);
    CHECK_TRUE(book.retainedRequests() == 0);
    r = request(1, state); r.resource = ayt::stats::statId("missing");
    CHECK_TRUE(settle(r, relations(), book, state, plan(), inputs).error().code == Error::Missing);
    CHECK_TRUE(state.read(ayt::stats::statId("health")).value().current == 100);
    CHECK_TRUE(book.close({1, 1}).has_value());
}

TEST_CASE(environment_damage_keeps_effect_provenance_without_actor) {
    auto state = stats(); auto book = ledger(); auto r = request(1, state);
    r.source = {.kind = SourceKind::Environment, .effect = 42};
    r.filter.allowed = mask(AllowedTarget::Environment);
    const std::array<ayt::stats::Value, 1> inputs{15};
    const auto outcome = settle(r, relations(), book, state, plan(), inputs).value();
    CHECK_TRUE(outcome.source.effect == 42 && !outcome.source.instigator);
    CHECK_TRUE(outcome.settlement.changes[0].actual == -15);
}

TEST_CASE(invulnerability_rejects_without_consuming_then_same_request_can_apply) {
    auto state = stats(); auto book = ledger(); auto r = request(1,state); auto rules = standardRules().value();
    const std::array<ayt::stats::Value,1> input{30}; r.facts.invulnerable = true;
    auto denied = settle(r,relations(),book,state,plan(),input,{&rules}).value();
    CHECK_TRUE(denied.kind == OutcomeKind::Rejected && denied.rule.reason == 1 && book.retainedRequests() == 0);
    r.facts.invulnerable = false;
    CHECK_TRUE(settle(r,relations(),book,state,plan(),input,{&rules})->kind == OutcomeKind::Damage);
}
TEST_CASE(block_confirms_hit_without_formula_or_resource_commit) {
    auto state = stats(); auto book = ledger(); auto r = request(1,state); auto rules = standardRules().value();
    r.facts.blocking = true; const auto version = state.version();
    auto blocked = settle(r,relations(),book,state,plan(),{}, {&rules}).value();
    CHECK_TRUE(blocked.kind == OutcomeKind::Blocked && blocked.rule.reason == 3 && blocked.settlement.changes.empty());
    CHECK_TRUE(state.version() == version && state.read(r.resource)->current == 100);
    CHECK_TRUE(settle(r,relations(),book,state,plan(),{}, {&rules}).error().code == Error::Duplicate);
}
TEST_CASE(life_uses_actual_depletion_once_and_explicit_revival_policy) {
    auto state = stats(); auto book = ledger({HitScope::Segment}); const auto resource = ayt::stats::statId("health");
    LifeTracker life({resource, LifePhase::Downed, true});
    const std::array<ayt::stats::Value,1> lethal{200}, zero{0}, heal{20};
    auto r = request(1,state);
    auto death = settle(r,relations(),book,state,plan(),lethal,{nullptr,&life}).value();
    CHECK_TRUE(death.kind == OutcomeKind::Damage && death.life->after == LifePhase::Downed && death.life->sequence == 1);
    r = request(2,state); r.hit.segment = 1;
    auto noDamage = settle(r,relations(),book,state,plan(),lethal,{nullptr,&life}).value();
    CHECK_TRUE(noDamage.kind == OutcomeKind::Zero && !noDamage.life);
    r = request(3,state); r.hit.segment = 2; r.filter.operation = Operation::Heal;
    CHECK_TRUE(settle(r,relations(),book,state,plan(),heal,{nullptr,&life})->life->after == LifePhase::Alive);
    CHECK_TRUE(life.phase() == LifePhase::Alive);
    r = request(4,state); r.hit.segment = 3;
    CHECK_TRUE(settle(r,relations(),book,state,plan(),zero,{nullptr,&life})->kind == OutcomeKind::Zero);
    r = request(5,state); r.hit.segment = 4;
    auto depletedAgain = settle(r,relations(),book,state,plan(),lethal,{nullptr,&life}).value();
    CHECK_TRUE(depletedAgain.life->after == LifePhase::Downed && depletedAgain.life->sequence == 3);
}
TEST_CASE(failed_commit_does_not_change_life_or_hit_confirmation) {
    struct RefusingTarget : SettlementTarget {
        ayt::stats::StatSet data = stats(); int commits = 0;
        Result<const ayt::stats::StatSet*> state() const override { return &data; }
        ayt::stats::Result<ayt::stats::Settlement> commit(std::span<const ayt::stats::Mutation>,std::uint64_t) override {
            ++commits; return std::unexpected(ayt::stats::Diagnostic{ayt::stats::Error::Stale,"owner rejected",{}});
        }
    } adapter;
    auto book = ledger(); auto r = request(1,adapter.data); LifeTracker life({r.resource});
    const std::array<ayt::stats::Value,1> lethal{200};
    CHECK_TRUE(settle(r,relations(),book,adapter,plan(),lethal,{nullptr,&life}).error().code == Error::Stale);
    CHECK_TRUE(adapter.commits == 1 && life.phase() == LifePhase::Alive && book.close({1,1}).has_value());
}
TEST_CASE(healing_does_not_implicitly_revive_with_default_policy) {
    auto state = stats(); auto book = ledger({HitScope::Segment}); auto r = request(1,state);
    LifeTracker life({r.resource}); const std::array<ayt::stats::Value,1> lethal{100}, healing{20};
    CHECK_TRUE(settle(r,relations(),book,state,plan(),lethal,{nullptr,&life})->life->after == LifePhase::Dead);
    r = request(2,state); r.hit.segment = 1; r.filter.operation = Operation::Heal;
    CHECK_FALSE(settle(r,relations(),book,state,plan(),healing,{nullptr,&life})->life.has_value());
    CHECK_TRUE(life.phase() == LifePhase::Dead && state.read(r.resource)->current == 20);
}
TEST_CASE(external_atomic_transaction_observes_final_life_state_instead_of_intermediate_zero) {
    auto state = stats(); const auto resource = ayt::stats::statId("health"); LifeTracker life({resource});
    auto mixed = state.commit(std::array{ayt::stats::Mutation{resource,ayt::stats::MutationOp::Decrease,100},
        ayt::stats::Mutation{resource,ayt::stats::MutationOp::Increase,20}},state.version()).value();
    CHECK_TRUE(state.read(resource)->current == 20 && !life.observe(mixed));
    CHECK_TRUE(life.phase() == LifePhase::Alive);
    auto lethal = state.commit(std::array{ayt::stats::Mutation{resource,ayt::stats::MutationOp::Increase,20},
        ayt::stats::Mutation{resource,ayt::stats::MutationOp::Decrease,40}},state.version()).value();
    CHECK_TRUE(life.observe(lethal)->after == LifePhase::Dead && state.read(resource)->current == 0);
}
TEST_CASE(life_tracking_ignores_base_and_capacity_fields_of_the_mapped_stat) {
    LifeTracker life({ayt::stats::statId("health")});
    ayt::stats::Settlement metadata{1,2,{{ayt::stats::statId("health"),100,0,-100,0,ayt::stats::ChangeField::Base},
        {ayt::stats::statId("health"),100,0,-100,0,ayt::stats::ChangeField::Capacity}}};
    CHECK_FALSE(life.observe(metadata).has_value());CHECK_TRUE(life.phase()==LifePhase::Alive);
    metadata.changes={{ayt::stats::statId("health"),100,0,-100,0,ayt::stats::ChangeField::Current}};
    CHECK_TRUE(life.observe(metadata)->after==LifePhase::Dead);
}
TEST_SUITE_END
