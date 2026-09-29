#include <AYCombat/EntityIntegration.h>
#include <AYEntity.h>
#include <AYEntity/ComponentRegistration.h>
#include <atomic>
#include <limits>

namespace ayt::combat {
AY_FINALIZE_REGISTRATION_METADATA(CombatComponent)
namespace {
std::atomic_uint64_t generations = 0;
auto fail(Error code, const char* message) { return std::unexpected(Diagnostic{code, message, {}}); }
struct Components { ayt::stats::StatsComponent* stats; CombatComponent* combat; };
Result<Components> resolve(ayt::entity::World& world, EntityBinding binding) {
    auto* system = dynamic_cast<ayt::stats::StatsSystem*>(world.findSystemByName("StatsSystem"));
    if (!system || system->worldToken() != binding.ref.domain || !binding.ref.valid() ||
        binding.ref.object > UINT32_MAX || binding.ref.generation - 1 > UINT32_MAX)
        return fail(Error::Stale, "combat World/handle token changed");
    auto* entity = world.getEntityByHandle({std::uint32_t(binding.ref.object), std::uint32_t(binding.ref.generation - 1)});
    if (!entity) return fail(Error::Stale, "combat Entity no longer exists");
    auto* stats = entity->getComponent<ayt::stats::StatsComponent>();
    auto* combat = entity->getComponent<CombatComponent>();
    if (!stats || !combat || stats->generation() != binding.statsGeneration || combat->generation() != binding.combatGeneration ||
        !combat->lifeMatches(stats->generation())) return fail(Error::Stale, "combat/Stats component incarnation changed");
    if (!stats->state()) return fail(Error::Missing, "Stats is not configured");
    if (!stats->authority()) return fail(Error::Invalid, "replica cannot settle authoritative combat");
    return Components{stats, combat};
}
class EntityTarget final : public SettlementTarget {
public:
    EntityTarget(ayt::entity::World& world, EntityBinding binding) : _world(world), _binding(binding) {}
    Result<const ayt::stats::StatSet*> state() const override {
        auto current = resolve(_world, _binding); if (!current) return std::unexpected(current.error());
        return current->stats->state();
    }
    ayt::stats::Result<ayt::stats::Settlement> commit(std::span<const ayt::stats::Mutation> mutations, std::uint64_t version) override {
        auto current = resolve(_world, _binding);
        if (!current) return std::unexpected(ayt::stats::Diagnostic{ayt::stats::Error::Stale, current.error().message, {}});
        return current->stats->commit(mutations, version, ayt::stats::NotificationDelivery::NextTick);
    }
private:
    ayt::entity::World& _world;
    EntityBinding _binding;
};
}
CombatComponent::CombatComponent() : _generation(++generations) {
    team = 0; invulnerable = false; blocking = false;
    capabilities = mask(Capability::Damageable) | mask(Capability::Healable);
}
Result<void> CombatComponent::configureLife(const ayt::stats::StatsComponent& stats, LifePolicy policy) {
    if (!policy.resource || (policy.depleted != LifePhase::Dead && policy.depleted != LifePhase::Downed))
        return fail(Error::Invalid, "invalid life policy");
    const auto* state = stats.state(); if (!state) return fail(Error::Missing, "Stats is not configured");
    auto index = state->schema()->index(policy.resource);
    if (!index || state->schema()->definitions()[*index].shape != ayt::stats::Shape::Pool ||
        state->schema()->definitions()[*index].lifetime != ayt::stats::Lifetime::Persistent)
        return fail(Error::Invalid, "life policy requires a persistent pool");
    auto row = stats.read(policy.resource); if (!row) return fail(Error::NumericFailure, "life resource read failed");
    _life.emplace(policy, row->current > 0 ? LifePhase::Alive : policy.depleted);
    _lifeStatsGeneration = stats.generation(); _generation = ++generations; return {};
}
CombatSystem::CombatSystem() : _rules(standardRules().value()) { _outcomes.reserve(OutcomeLimit); }
void CombatSystem::bind(ayt::entity::World& world, std::uint64_t token) noexcept { _world = &world; _token = token; }
Result<void> CombatSystem::setRules(RuleRegistry rules) {
    if (_settling) return fail(Error::Reentrant, "cannot change rules during settlement");
    _rules = std::move(rules); return {};
}
Result<CombatOutcome> CombatSystem::submit(EntityBinding binding, SettlementRequest request,
    const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs) {
    if (_settling) return fail(Error::Reentrant, "combat settlement is already in progress");
    if (!_world || _token != binding.ref.domain || request.hit.target != binding.ref)
        return fail(Error::Stale, "request does not match the bound World/Entity");
    auto current = resolve(*_world, binding); if (!current) return std::unexpected(current.error());
    if (_outcomes.size() == OutcomeLimit) return fail(Error::Limit, "drain combat outcomes before further settlement");
    CombatOutcome notification;
    notification.settlement.changes.reserve(1); // This adapter settles exactly one resource.
    // Allocate the notification copy before committing. The committed result moves into this slot.
    // The request's target/facts are overwritten with the current authoritative component values.
    request.target = {binding.ref, current->combat->team, current->combat->capabilities, true};
    request.facts = {current->combat->invulnerable, current->combat->blocking, current->combat->immuneTypes};
    _settling = true;
    struct Guard { bool& busy; ~Guard() { busy = false; } } guard{_settling};
    EntityTarget target(*_world, binding);
    auto outcome = settle(request, _relations, _hits, target, plan, inputs, {&_rules, current->combat->life()});
    if (!outcome) return outcome;
    notification = *outcome; // All scalar fields; change vector capacity was reserved before commit.
    _outcomes.push_back(std::move(notification));
    return outcome;
}
std::vector<CombatOutcome> CombatSystem::takeOutcomes() {
    if (_settling) return {};
    std::vector<CombatOutcome> result; result.reserve(_outcomes.size());
    for (auto& outcome : _outcomes) result.push_back(std::move(outcome));
    _outcomes.clear(); return result;
}
Result<EntityBinding> bindCombatant(ayt::entity::World& world, ayt::entity::EntityHandle handle) {
    auto* system = dynamic_cast<ayt::stats::StatsSystem*>(world.findSystemByName("StatsSystem"));
    auto* entity = world.getEntityByHandle(handle);
    if (!system || !entity) return fail(Error::Stale, "World/Entity is unavailable");
    auto* stats = entity->getComponent<ayt::stats::StatsComponent>(); auto* combat = entity->getComponent<CombatComponent>();
    if (!stats || !combat) return fail(Error::Missing, "Entity requires Stats and Combat components");
    EntityBinding binding{{system->worldToken(), handle.id, std::uint64_t(handle.version) + 1}, stats->generation(), combat->generation()};
    auto current = resolve(world, binding); if (!current) return std::unexpected(current.error()); return binding;
}
Result<void> registerCombatComponent(ayt::entity::ComponentRegistry& registry) {
    auto result = ayt::entity::registerSceneComponent<CombatComponent>(registry, "CombatComponent", "Combat", "Gameplay");
    if (!result) return std::unexpected(Diagnostic{Error::Invalid, result.message(), {}}); return {};
}
Result<void> registerCombatSystem(ayt::entity::World& world) {
    auto* stats = dynamic_cast<ayt::stats::StatsSystem*>(world.findSystemByName("StatsSystem"));
    if (!stats) return fail(Error::Missing, "install StatsSystem before CombatSystem");
    if (!world.findSystemByName("CombatSystem")) world.registerSystem<CombatSystem>(130, ayt::entity::SystemLane::Sim);
    auto* system = dynamic_cast<CombatSystem*>(world.findSystemByName("CombatSystem"));
    if (!system) return fail(Error::Invalid, "CombatSystem name collision");
    system->bind(world, stats->worldToken()); return {};
}
} // namespace ayt::combat
