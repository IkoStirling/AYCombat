#include <AYCombat/StatsIntegration.h>
#include <array>
#include <iostream>
#include <stdexcept>

template<class T> T checked(ayt::stats::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
int main() {
    try {
        auto schema = checked(ayt::stats::Schema::compile({ayt::stats::Definition{
            .name = "health", .shape = ayt::stats::Shape::Pool, .current = 100, .capacity = 100}}));
        auto state = checked(ayt::stats::StatSet::create(schema));
        auto formula = checked(ayt::stats::CalculationPlan::compile({
            {.id = 1, .op = ayt::stats::Op::Input, .binding = 0}}, 1, 1));
        ayt::combat::RelationTable relations;
        if (!relations.set(1, 2, ayt::combat::Relation::Hostile)) return 1;
        ayt::combat::HitLedger ledger;
        if (!ledger.open({1, 1}, {}, 0, 100)) return 1;
        const ayt::combat::CombatantRef attacker{1, 1, 1}, target{1, 2, 1};
        ayt::combat::SettlementRequest request{
            .hit = {{1, 1}, 1, target, 0},
            .source = {.instigator = attacker, .attribution = 1, .team = 1},
            .target = {target, 2, ayt::combat::mask(ayt::combat::Capability::Damageable)},
            .resource = ayt::stats::statId("health"), .expectedVersion = state.version(),
            .calculation = {.seed = 123, .calculation = 1, .hit = 1}};
        const std::array<ayt::stats::Value, 1> inputs{30};
        auto result = ayt::combat::settle(request, relations, ledger, state, formula, inputs);
        if (!result) { std::cerr << result.error().message << '\n'; return 1; }
        if (result->state != ayt::combat::OutcomeState::Applied) return 1;
        std::cout << "actual damage: " << -result->settlement.changes[0].actual
                  << ", health: " << checked(state.read(request.resource)).current << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
