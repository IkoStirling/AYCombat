#include <AYTest.h>
#include <AYCombat/EntityIntegration.h>
#include <AYEntity.h>
#include <AYEntity/EntityModule.h>
#include <array>
#include <stdexcept>
using namespace ayt::combat;
namespace {
struct Fixture {
    ayt::entity::World& world = ayt::entity::World::processWorld();
    ayt::entity::Entity* target = nullptr;
    ayt::stats::StatsComponent* stats = nullptr;
    CombatComponent* combat = nullptr;
    EntityBinding binding;
    SourceContext source;
    Fixture() {
        ayt::entity::World::setActiveWorld(nullptr); world.initialize();
        auto& registry = ayt::entity::ComponentRegistry::instance();
        if (!registry.isSealed()) {
            (void)ayt::entity::registerEntityCoreComponents(registry);
            if (!ayt::stats::registerStatsComponent(registry) || !registerCombatComponent(registry)) throw std::runtime_error("component registration");
            registry.seal();
        }
        if (!ayt::stats::registerStatsSystem(world) || !registerCombatSystem(world)) throw std::runtime_error("system registration");
        target = world.createEntity(); stats = target->addComponent<ayt::stats::StatsComponent>();
        if (!stats->configure(schema())) throw std::runtime_error("Stats configuration");
        combat = target->addComponent<CombatComponent>(); combat->team = 20;
        if (!combat->configureLife(*stats,{ayt::stats::statId("health")})) throw std::runtime_error("life policy");
        binding = bindCombatant(world,world.getEntityHandle(target->getId())).value();
        auto* attacker = world.createEntity(); const auto handle = world.getEntityHandle(attacker->getId());
        source.instigator = CombatantRef{binding.ref.domain,handle.id,std::uint64_t(handle.version)+1}; source.team = 10; source.attribution = 42;
        if (!system().relations().set(10,20,Relation::Hostile) || !system().hits().open({binding.ref.domain,1},{HitScope::Segment},0,100)) throw std::runtime_error("combat setup");
    }
    ~Fixture() { world.shutdown(); }
    static std::shared_ptr<const ayt::stats::Schema> schema() {
        return ayt::stats::Schema::compile({{.name="health",.shape=ayt::stats::Shape::Pool,.current=100,.capacity=100}}).value();
    }
    CombatSystem& system() { return *dynamic_cast<CombatSystem*>(world.findSystemByName("CombatSystem")); }
    SettlementRequest request(std::uint64_t id=1) {
        SettlementRequest r; r.hit = {{binding.ref.domain,1},id,binding.ref,0,0,std::uint32_t(id)};
        r.source = source; r.resource = ayt::stats::statId("health"); r.expectedVersion = stats->version();
        r.calculation = {.seed=7,.calculation=100+id,.hit=200+id}; return r;
    }
    static ayt::stats::CalculationPlan plan() { return ayt::stats::CalculationPlan::compile({{.id=1,.op=ayt::stats::Op::Input,.binding=0}},1,1).value(); }
    Result<CombatOutcome> submit(SettlementRequest r, ayt::stats::Value amount=30) { return system().submit(binding,std::move(r),plan(),std::array{amount}); }
};
}
TEST_SUITE(CombatEntityIntegrationTests)
TEST_CASE(default_component_properties_and_initial_zero_life_are_explicit) {
    CombatComponent fresh;
    CHECK_TRUE(fresh.team == 0 && !fresh.invulnerable && !fresh.blocking);
    CHECK_TRUE(fresh.capabilities == (mask(Capability::Damageable)|mask(Capability::Healable)));
    Fixture f;
    CHECK_TRUE(f.stats->commit(std::array{ayt::stats::Mutation{ayt::stats::statId("health"),ayt::stats::MutationOp::Decrease,100}},f.stats->version()).has_value());
    CHECK_TRUE(f.combat->configureLife(*f.stats,{ayt::stats::statId("health")}).has_value());
    f.binding = bindCombatant(f.world,f.world.getEntityHandle(f.target->getId())).value();
    CHECK_TRUE(f.combat->life()->phase() == LifePhase::Dead);
    auto zero = f.submit(f.request()).value(); CHECK_TRUE(zero.kind == OutcomeKind::Zero && !zero.life);
}
TEST_CASE(enemy_invulnerability_removal_damage_and_single_death_confirmation) {
    Fixture f;
    auto first = f.submit(f.request()).value(); CHECK_TRUE(first.kind == OutcomeKind::Damage && first.settlement.changes[0].actual == -30);
    auto r = f.request(2); f.combat->invulnerable = true;
    auto denied = f.submit(r).value(); CHECK_TRUE(denied.kind == OutcomeKind::Rejected && denied.rule.reason == 1);
    CHECK_TRUE(f.stats->read(r.resource)->current == 70);
    f.combat->invulnerable = false;
    auto death = f.submit(r,100).value(); CHECK_TRUE(death.life->after == LifePhase::Dead && death.settlement.changes[0].actual == -70);
    auto zero = f.submit(f.request(3),100).value(); CHECK_TRUE(zero.kind == OutcomeKind::Zero && !zero.life);
    auto events = f.system().takeOutcomes(); CHECK_TRUE(events.size() == 4 && events[2].source.attribution == 42);
    CHECK_TRUE(f.system().outcomes().empty());
}
TEST_CASE(current_authoritative_facts_override_a_forged_request_view) {
    Fixture f; f.combat->immuneTypes = {8}; auto r = f.request(); r.damageType = 8;
    r.facts.invulnerable = false; r.target.team = 10;
    CHECK_TRUE(f.submit(r)->rule.reason == 2);
    f.combat->immuneTypes.clear(); f.combat->blocking = true;
    const auto version = f.stats->version(); CHECK_TRUE(f.submit(r)->kind == OutcomeKind::Blocked);
    CHECK_TRUE(f.stats->version() == version);
    CHECK_TRUE(f.submit(r).error().code == Error::Duplicate);
}
TEST_CASE(deleted_entity_rejects_late_work_and_retained_outcome_owns_source) {
    Fixture f; auto r = f.request();
    auto* attacker = f.world.getEntityByHandle({std::uint32_t(f.source.instigator->object),std::uint32_t(f.source.instigator->generation-1)});
    f.world.destroyEntity(attacker); CHECK_TRUE(f.submit(r).has_value());
    f.world.destroyEntity(f.target);
    CHECK_TRUE(f.system().submit(f.binding,r,Fixture::plan(),std::array<ayt::stats::Value,1>{30}).error().code == Error::Stale);
    auto events = f.system().takeOutcomes(); CHECK_TRUE(events.size() == 1 && events[0].source.instigator.has_value());
}
TEST_CASE(component_replacement_and_stats_reconfiguration_invalidate_binding) {
    Fixture f; auto r = f.request(); f.target->removeComponent<CombatComponent>();
    f.combat = f.target->addComponent<CombatComponent>(); f.combat->team = 20;
    CHECK_TRUE(f.submit(r).error().code == Error::Stale);
    f.binding = bindCombatant(f.world,f.world.getEntityHandle(f.target->getId())).value();
    r = f.request(); CHECK_TRUE(f.stats->configure(Fixture::schema()).has_value());
    CHECK_TRUE(f.submit(r).error().code == Error::Stale);
    CHECK_TRUE(f.stats->read(r.resource)->current == 100 && f.system().outcomes().empty());
}
TEST_CASE(world_restart_rejects_old_binding_even_when_entity_ids_are_reused) {
    Fixture f; auto r = f.request(); const auto old = f.binding;
    f.world.shutdown(); f.world.initialize();
    CHECK_TRUE(ayt::stats::registerStatsSystem(f.world).has_value()); CHECK_TRUE(registerCombatSystem(f.world).has_value());
    CHECK_TRUE(f.system().submit(old,r,Fixture::plan(),std::array<ayt::stats::Value,1>{30}).error().code == Error::Stale);
}
TEST_CASE(replica_and_stale_version_do_not_publish_outcomes_or_consume_hit) {
    Fixture f; auto r = f.request(); f.stats->setAuthority(false);
    CHECK_FALSE(f.submit(r).has_value()); f.stats->setAuthority(true); --r.expectedVersion;
    CHECK_TRUE(f.submit(r).error().code == Error::Stale && f.system().outcomes().empty());
    r.expectedVersion = f.stats->version(); CHECK_TRUE(f.submit(r).has_value());
    CHECK_TRUE(f.stats->wireState.find("70") != std::string::npos);
}
TEST_CASE(life_binding_automatically_renews_after_stats_reset) {
    Fixture f; CHECK_TRUE(f.stats->configure(Fixture::schema()).has_value());
    CHECK_TRUE(bindCombatant(f.world,f.world.getEntityHandle(f.target->getId())).has_value());
    CHECK_TRUE(f.combat->configureLife(*f.stats,{ayt::stats::statId("health"),LifePhase::Downed,true}).has_value());
    f.binding = bindCombatant(f.world,f.world.getEntityHandle(f.target->getId())).value();
    auto downed = f.submit(f.request(),100);
    if (!downed) throw std::runtime_error(downed.error().message);
    CHECK_TRUE(downed->life.has_value());
    if (downed->life) CHECK_TRUE(downed->life->after == LifePhase::Downed);
    auto heal = f.request(2); heal.filter.operation = Operation::Heal;
    auto revived = f.submit(heal,20);
    if (!revived) throw std::runtime_error(revived.error().message);
    CHECK_TRUE(revived->life.has_value());
    if (revived->life) CHECK_TRUE(revived->life->after == LifePhase::Alive);
}
TEST_CASE(rule_reentry_is_rejected_and_notification_consumers_run_after_commit) {
    Fixture f; auto r = f.request(); bool reentryRejected = false;
    auto rules = RuleRegistry::compile({{99,RuleStage::Permission,0,RuleAction::Reject,[&](const RuleContext&) {
        reentryRejected = f.submit(r).error().code == Error::Reentrant; return false;
    }}}).value();
    CHECK_TRUE(f.system().setRules(std::move(rules)).has_value()); CHECK_TRUE(f.submit(r).has_value());
    CHECK_TRUE(reentryRejected && f.system().outcomes().size() == 1);
    auto events = f.system().takeOutcomes(); CHECK_TRUE(events[0].settlement.changes[0].after == f.stats->read(r.resource)->current);
}
TEST_CASE(outcome_queue_limit_rejects_before_commit_and_drain_allows_retry) {
    Fixture f; auto r = f.request(); f.combat->invulnerable = true;
    for (int i=0;i<1024;++i) if (!f.submit(r)) throw std::runtime_error("outcome queue filled early");
    f.combat->invulnerable = false; const auto version = f.stats->version();
    CHECK_TRUE(f.submit(r).error().code == Error::Limit && f.stats->version() == version);
    CHECK_TRUE(f.system().takeOutcomes().size() == 1024); CHECK_TRUE(f.submit(r).has_value());
}
TEST_CASE(stats_observer_runs_next_tick_after_hit_and_life_confirmation) {
    Fixture f; auto r = f.request(); int notifications = 0;
    f.stats->setOnSettled([&](const ayt::stats::Settlement&) {
        ++notifications;
        if (f.combat->life()->phase() != LifePhase::Dead || f.system().outcomes().size() != 1) throw std::runtime_error("premature notification");
        if (f.submit(r).error().code != Error::Duplicate) throw std::runtime_error("hit was not completed");
        throw std::runtime_error("consumer failed after commit");
    });
    CHECK_TRUE(f.submit(r,100).has_value()); CHECK_TRUE(notifications == 0);
    f.world.fixedUpdate(.016f); CHECK_TRUE(notifications == 1);
    CHECK_TRUE(f.stats->lastError().has_value() && f.stats->read(r.resource)->current == 0);
    CHECK_TRUE(f.submit(r).error().code == Error::Duplicate);
    f.world.fixedUpdate(.016f); CHECK_TRUE(notifications == 1);
}
TEST_CASE(stats_notification_backpressure_fails_before_damage_and_can_retry_after_tick) {
    Fixture f;
    for(std::uint64_t i=1;i<=1024;++i) {
        if(!f.submit(f.request(i),0)) throw std::runtime_error("deferred queue filled early");
        (void)f.system().takeOutcomes();
    }
    auto r = f.request(1025); const auto version = f.stats->version();
    CHECK_TRUE(f.submit(r).error().code == Error::Limit && f.stats->version() == version);
    CHECK_TRUE(f.system().outcomes().empty());
    f.world.fixedUpdate(.016f); r.expectedVersion = f.stats->version();
    CHECK_TRUE(f.submit(r)->kind == OutcomeKind::Damage && f.stats->read(r.resource)->current == 70);
}
TEST_SUITE_END
