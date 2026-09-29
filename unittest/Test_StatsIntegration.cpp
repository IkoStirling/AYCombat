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
TEST_SUITE_END
