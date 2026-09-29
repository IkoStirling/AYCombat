#include <AYTest.h>
#include <AYCombat/StatsIntegration.h>
#include "Fixtures.h"
#include <array>

namespace {
ayt::stats::StatSet stats() {
    auto schema = ayt::stats::Schema::compile({ayt::stats::Definition{
        .name = "health", .shape = ayt::stats::Shape::Pool, .current = 100, .capacity = 100}}).value();
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

TEST_SUITE_END
