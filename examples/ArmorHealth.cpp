#include <AYCombat/StatsIntegration.h>

#include <array>
#include <iostream>

int main() {
    using namespace ayt;
    using namespace combat;
    auto schema = stats::Schema::compile({
        stats::Definition{.name="armor", .shape=stats::Shape::Pool,
            .current=40, .capacity=40},
        stats::Definition{.name="health", .shape=stats::Shape::Pool,
            .current=100, .capacity=100}});
    if (!schema) return 1;
    auto state = stats::StatSet::create(*schema);
    if (!state) return 1;
    RelationTable relations;
    if (!relations.set(1, 2, Relation::Hostile)) return 1;
    HitLedger ledger;
    if (!ledger.open({1, 1}, {}, 0, 100)) return 1;
    const CombatantRef attacker{1, 1, 1}, victim{1, 2, 1};
    SettlementRequest request{
        .hit = {{1, 1}, 1, victim, 0},
        .source = {.instigator=attacker, .attribution=1, .team=1},
        .target = {victim, 2, mask(Capability::Damageable)},
        .resource = stats::statId("health"), .expectedVersion = state->version(),
        .calculation = {.seed=7, .calculation=1, .hit=1}};
    request.hit.part = 1;
    // The game maps the collider to part 1 and authors the 50% absorption rate.
    const PartArmorRoute route{1, stats::statId("health"),
        stats::statId("armor"), 5000};
    auto outcome = settlePartArmorHit(request, relations, ledger, *state,
        route, 30);
    if (!outcome || outcome->state != OutcomeState::Applied) return 1;
    const auto armor = state->read(stats::statId("armor"));
    const auto health = state->read(stats::statId("health"));
    if (!armor || !health || armor->current != 25 || health->current != 85)
        return 1;
    std::cout << "one hit: armor=" << armor->current
              << " health=" << health->current << '\n';
    return 0;
}
