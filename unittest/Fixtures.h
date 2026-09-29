#pragma once
#include <AYCombat/HitLedger.h>
#include <stdexcept>
using namespace ayt::combat;
inline CombatantRef actor(std::uint64_t id = 1, std::uint64_t generation = 1) { return {1, id, generation}; }
inline SourceContext source() { return {.instigator = actor(), .directSource = actor(50), .attribution = 100, .team = 10}; }
inline CombatantView target() { return {actor(2), 20, mask(Capability::Damageable) | mask(Capability::Healable)}; }
inline RelationTable relations() {
    RelationTable table;
    if (!table.set(10, 20, Relation::Hostile)) throw std::runtime_error("fixture relationship failed");
    return table;
}
inline HitRequest hit(std::uint64_t request = 1, std::uint64_t tick = 0) { return {{1, 1}, request, actor(2), tick}; }
inline HitLedger ledger(HitPolicy policy = {}) {
    HitLedger result;
    if (!result.open({1, 1}, policy, 0, 100)) throw std::runtime_error("fixture attack failed");
    return result;
}
