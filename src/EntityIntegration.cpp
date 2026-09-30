#include <AYCombat/EntityIntegration.h>
#include <AYEntity.h>
#include <AYEntity/ComponentRegistration.h>
#include <AYEntity/EntityRuntimeModule.h>
#include <atomic>
#include <limits>

namespace ayt::combat {
AY_FINALIZE_REGISTRATION_METADATA(CombatComponent)
namespace {
std::atomic_uint64_t generations = 0;
auto fail(Error code, const char* message) { return std::unexpected(Diagnostic{code, message, {}}); }
ActionResult gameplayResult(const CombatOutcome& outcome) noexcept {
    ActionResult result{outcome.kind,outcome.calculated,0,outcome.remaining,0,outcome.attack.id,outcome.decision,outcome.rule,outcome.life};
    if(!outcome.settlement.changes.empty()) {
        const auto& change=outcome.settlement.changes.front();
        result.actual=outcome.operation==Operation::Damage?-change.actual:change.actual;result.excess=change.excess;
    }
    return result;
}
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
/// World-owned registration is weak in Stats; destruction leaves no dangling callback.
class StatsPublicationBridge final : public ayt::stats::StateObserver {
public:
    explicit StatsPublicationBridge(CombatSystem& owner):_owner(owner){}
    ayt::stats::Result<void> prepare(ayt::entity::EntityHandle handle,const ayt::stats::StatsComponent&,
        const ayt::stats::Schema& schema,ayt::stats::PublicationReason reason,std::size_t publications) override {
        auto* entity=_owner._world->getEntityByHandle(handle);
        auto* combat=entity?entity->getComponent<CombatComponent>():nullptr;
        if(!combat)return {};
        const auto resource=combat->lifeResourceName.empty()?combat->lifeResource:ayt::stats::statId(combat->lifeResourceName);
        if(!resource)return {};
        if(combat->depletedPhase!=std::uint32_t(LifePhase::Dead) && combat->depletedPhase!=std::uint32_t(LifePhase::Downed))
            return std::unexpected(ayt::stats::Diagnostic{ayt::stats::Error::Invalid,"invalid authored depleted phase",{}});
        auto index=schema.index(resource);
        if(!index || schema.definitions()[*index].shape!=ayt::stats::Shape::Pool ||
            schema.definitions()[*index].lifetime!=ayt::stats::Lifetime::Persistent)
            return std::unexpected(ayt::stats::Diagnostic{ayt::stats::Error::Invalid,"configured life pool is missing/incompatible",{}});
        if(reason!=ayt::stats::PublicationReason::Configure && reason!=ayt::stats::PublicationReason::Restore &&
            reason!=ayt::stats::PublicationReason::Replica && publications>CombatSystem::OutcomeLimit-_owner._lifeEvents.size())
            return std::unexpected(ayt::stats::Diagnostic{ayt::stats::Error::Limit,"life notification capacity exhausted",{}});
        return {};
    }
    void published(ayt::entity::EntityHandle handle,const ayt::stats::StatsComponent& stats,
        ayt::stats::PublicationReason reason) noexcept override {
        auto* entity=_owner._world->getEntityByHandle(handle);
        auto* combat=entity?entity->getComponent<CombatComponent>():nullptr;
        if(!combat)return;
        const auto sequence=combat->life()?combat->life()->sequence():0;
        combat->synchronizeLife(stats,reason);
        if(combat->life() && combat->life()->sequence()!=sequence && combat->_lastTransition) {
            if(_owner._lifeEvents.size()==CombatSystem::OutcomeLimit)std::terminate();
            _owner._lifeEvents.push_back({handle,*combat->_lastTransition,stats.generation(),combat->generation()});
        }
    }
private:
    CombatSystem& _owner;
};
CombatComponent::CombatComponent() : _generation(++generations) {
    team = 0; invulnerable = false; blocking = false;
    lifeResource=0;depletedPhase=std::uint32_t(LifePhase::Dead);healingRevives=false;
    capabilities = mask(Capability::Damageable) | mask(Capability::Healable);
}
void CombatComponent::onAttach(ayt::entity::Entity* owner) {
    _owner=owner;
    if(auto* world=owner->getWorld())if(auto* system=dynamic_cast<CombatSystem*>(world->findSystemByName("CombatSystem")))
        system->initializeComponent(world->getEntityHandle(owner->getId()),*this);
}
void CombatComponent::onStart() {
    if(_owner)if(auto* stats=_owner->getComponent<ayt::stats::StatsComponent>())synchronizeLife(*stats,ayt::stats::PublicationReason::Configure);
}
void CombatComponent::synchronizeLife(const ayt::stats::StatsComponent& stats,ayt::stats::PublicationReason reason) noexcept {
    if(reason==ayt::stats::PublicationReason::Detach) {_lifeStatsGeneration=0;_life.reset();_lastTransition.reset();return;}
    if(!lifeResourceName.empty())lifeResource=ayt::stats::statId(lifeResourceName);
    if(lifeResource && (depletedPhase==std::uint32_t(LifePhase::Dead) || depletedPhase==std::uint32_t(LifePhase::Downed)))
        _policy={lifeResource,LifePhase(depletedPhase),healingRevives};
    const auto* state=stats.state();if(!state || !_policy.resource)return;
    auto index=state->schema()->index(_policy.resource);
    if(!index || state->schema()->definitions()[*index].shape!=ayt::stats::Shape::Pool)return;
    const auto current=state->rows()[*index].current;
    if(!_life || stats.generation()!=_lifeStatsGeneration || reason==ayt::stats::PublicationReason::Replica) {
        _life.emplace(_policy,current>0?LifePhase::Alive:_policy.depleted);
        _current=current;_lifeStatsGeneration=stats.generation();_lastTransition.reset();return;
    }
    _lastTransition=_life->observeCurrent(_current,current);_current=current;
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
    _policy=policy;_current=row->current;_lastTransition.reset();
    lifeResource=policy.resource;depletedPhase=std::uint32_t(policy.depleted);healingRevives=policy.healingRevives;
    lifeResourceName=state->schema()->definitions()[*index].name;
    _life.emplace(policy, row->current > 0 ? LifePhase::Alive : policy.depleted);
    _lifeStatsGeneration = stats.generation(); _generation = ++generations; return {};
}
CombatSystem::CombatSystem() : _rules(standardRules().value()) {
    _outcomes.reserve(OutcomeLimit);_lifeEvents.reserve(OutcomeLimit);_actors.reserve(ayt::entity::MAX_ENTITIES);
}
Result<void> CombatSystem::bind(ayt::entity::World& world,ayt::stats::StatsSystem& stats) {
    if(_world && (_world!=&world || _token!=stats.worldToken()))return fail(Error::Stale,"cannot move a combat owner between Worlds");
    _world=&world;_token=stats.worldToken();_stats=&stats;
    if(!_publication)_publication=std::make_shared<StatsPublicationBridge>(*this);
    auto result=stats.observeState(_publication);
    if(!result)return std::unexpected(Diagnostic{Error::Invalid,result.error().message,result.error().path});
    return {};
}
Result<void> CombatSystem::setRules(RuleRegistry rules) {
    if (_settling || _delivering) return fail(Error::Reentrant, "cannot change rules during settlement/delivery");
    _rules = std::move(rules); return {};
}
Result<CombatOutcome> CombatSystem::submit(EntityBinding binding, SettlementRequest request,
    const ayt::stats::CalculationPlan& plan, std::span<const ayt::stats::Value> inputs) {
    if (_settling || _delivering) return fail(Error::Reentrant, "combat settlement/delivery is already in progress");
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
    const auto sequence=current->combat->life()?current->combat->life()->sequence():0;
    auto outcome = settle(request, _relations, _hits, target, plan, inputs, {&_rules, nullptr});
    if (!outcome) return outcome;
    if(current->combat->life() && current->combat->life()->sequence()!=sequence)outcome->life=current->combat->_lastTransition;
    if(auto resource=current->stats->state()->schema()->index(request.resource))outcome->remaining=current->stats->state()->rows()[*resource].current;
    notification = *outcome; // All scalar fields; change vector capacity was reserved before commit.
    _outcomes.push_back(std::move(notification));
    return outcome;
}
std::vector<CombatOutcome> CombatSystem::takeOutcomes() {
    if (_settling || _delivering) return {};
    std::vector<CombatOutcome> result; result.reserve(_outcomes.size());
    for (auto& outcome : _outcomes) result.push_back(std::move(outcome));
    _outcomes.clear(); return result;
}
std::vector<LifeEvent> CombatSystem::takeLifeEvents() {
    if(_settling || _delivering)return {};
    std::vector<LifeEvent> events;events.reserve(_lifeEvents.size());
    for(auto& event:_lifeEvents)events.push_back(std::move(event));
    _lifeEvents.clear();return events;
}
void CombatSystem::onUpdate(float) {
    if(!_world || _settling || _delivering)return;
    std::erase_if(_actors,[&](const auto& record){return !_world->getEntityByHandle(record.handle);});
    if(!_automatic)return;
    _delivering=true;struct Guard{bool& flag;~Guard(){flag=false;}} guard{_delivering};
    auto outcomeCallback=_onOutcome;auto lifeCallback=_onLife;
    for(const auto& outcome:_outcomes)if(outcomeCallback)try{outcomeCallback(gameplayResult(outcome));}catch(...){_error=Diagnostic{Error::Invalid,"outcome observer threw after commit",{}};}
    for(const auto& event:_lifeEvents)if(lifeCallback)try{lifeCallback(event);}catch(...){_error=Diagnostic{Error::Invalid,"life observer threw after commit",{}};}
    _outcomes.clear();_lifeEvents.clear();
}
void CombatSystem::initializeComponent(ayt::entity::EntityHandle handle,CombatComponent& component) {
    for(const auto& record:_actors)if(record.handle==handle) {
        component.team=record.team;component.capabilities=record.capabilities;component._policy=record.life;
        component.lifeResource=record.life.resource;component.depletedPhase=std::uint32_t(record.life.depleted);component.healingRevives=record.life.healingRevives;
        auto* entity=_world->getEntityByHandle(handle);
        if(auto* stats=entity?entity->getComponent<ayt::stats::StatsComponent>():nullptr) {
            if(stats->state())if(auto index=stats->state()->schema()->index(record.life.resource))component.lifeResourceName=stats->state()->schema()->definitions()[*index].name;
            component.synchronizeLife(*stats,ayt::stats::PublicationReason::Configure);
        }
        return;
    }
}
Result<void> CombatSystem::configureActor(ayt::entity::EntityHandle handle,const ActorProfile& profile) {
    if(_settling || _delivering)return fail(Error::Reentrant,"actor configuration during settlement/delivery");
    auto* entity=_world?_world->getEntityByHandle(handle):nullptr;
    if(!entity)return fail(Error::Stale,"actor Entity is unavailable");
    if(!profile.schema || !profile.life.resource || (profile.life.depleted!=LifePhase::Dead && profile.life.depleted!=LifePhase::Downed) ||
        (profile.capabilities&~(mask(Capability::Damageable)|mask(Capability::Healable))))return fail(Error::Invalid,"invalid actor profile");
    auto index=profile.schema->index(profile.life.resource);
    if(!index || profile.schema->definitions()[*index].shape!=ayt::stats::Shape::Pool ||
        profile.schema->definitions()[*index].lifetime!=ayt::stats::Lifetime::Persistent)return fail(Error::Invalid,"actor life requires a persistent pool");
    if(!ayt::entity::World::isComponentTypeRegistered<ayt::stats::StatsComponent>() || !ayt::entity::World::isComponentTypeRegistered<CombatComponent>())
        return fail(Error::Missing,"register gameplay components before registry seal");
    auto* existing=entity->getComponent<ayt::stats::StatsComponent>();
    if(existing && !existing->authority())return fail(Error::Invalid,"cannot configure a replica actor");
    std::erase_if(_actors,[&](const auto& record){return !_world->getEntityByHandle(record.handle);});
    auto record=std::find_if(_actors.begin(),_actors.end(),[&](const auto& r){return r.handle==handle;});
    if(record==_actors.end() && _actors.size()>=ayt::entity::MAX_ENTITIES)return fail(Error::Limit,"actor configuration capacity");
    auto* stats=entity->addComponent<ayt::stats::StatsComponent>();auto* combat=entity->addComponent<CombatComponent>();
    const auto previous=combat->_policy;const auto previousResource=combat->lifeResource;
    const auto previousPhase=combat->depletedPhase;const auto previousRevives=combat->healingRevives;
    const auto previousName=combat->lifeResourceName;
    combat->_policy=profile.life;combat->lifeResource=profile.life.resource;
    combat->lifeResourceName=profile.schema->definitions()[*index].name;
    combat->depletedPhase=std::uint32_t(profile.life.depleted);combat->healingRevives=profile.life.healingRevives;
    auto configured=stats->configure(profile.schema);
    if(!configured){combat->_policy=previous;combat->lifeResource=previousResource;combat->depletedPhase=previousPhase;combat->healingRevives=previousRevives;
        combat->lifeResourceName=previousName;
        return std::unexpected(Diagnostic{Error::NumericFailure,configured.error().message,configured.error().path});}
    // Validation above guarantees the mapping; this step cannot fail after numerical publication.
    if(!combat->configureLife(*stats,profile.life))std::terminate();
    combat->team=profile.team;combat->capabilities=profile.capabilities;
    ActorRecord saved{handle,profile.life,profile.team,profile.capabilities};
    if(record==_actors.end())_actors.push_back(saved);else *record=saved;
    _automatic=true;return {};
}
Result<void> CombatSystem::attachActor(ayt::entity::EntityHandle handle,LifePolicy life,std::uint64_t team,std::uint32_t capabilities) {
    if(_settling || _delivering)return fail(Error::Reentrant,"actor attachment during settlement/delivery");
    auto* entity=_world?_world->getEntityByHandle(handle):nullptr;
    if(!entity)return fail(Error::Stale,"actor Entity is unavailable");
    auto* stats=entity->getComponent<ayt::stats::StatsComponent>();
    if(!stats || !stats->state())return fail(Error::Missing,"actor requires configured Stats");
    if(!stats->authority())return fail(Error::Invalid,"cannot attach a replica actor");
    if(!life.resource || (life.depleted!=LifePhase::Dead && life.depleted!=LifePhase::Downed) ||
        (capabilities&~(mask(Capability::Damageable)|mask(Capability::Healable))))return fail(Error::Invalid,"invalid actor policy");
    const auto index=stats->state()->schema()->index(life.resource);
    if(!index || stats->state()->schema()->definitions()[*index].shape!=ayt::stats::Shape::Pool ||
        stats->state()->schema()->definitions()[*index].lifetime!=ayt::stats::Lifetime::Persistent)
        return fail(Error::Invalid,"actor life requires an existing persistent pool");
    if(!ayt::entity::World::isComponentTypeRegistered<CombatComponent>())
        return fail(Error::Missing,"register Combat component before registry seal");
    std::erase_if(_actors,[&](const auto& record){return !_world->getEntityByHandle(record.handle);});
    auto record=std::find_if(_actors.begin(),_actors.end(),[&](const auto& r){return r.handle==handle;});
    if(record==_actors.end() && _actors.size()>=ayt::entity::MAX_ENTITIES)return fail(Error::Limit,"actor configuration capacity");
    auto* combat=entity->addComponent<CombatComponent>();
    auto configured=combat->configureLife(*stats,life);
    if(!configured)return configured;
    combat->team=team;combat->capabilities=capabilities;
    ActorRecord saved{handle,life,team,capabilities};
    if(record==_actors.end())_actors.push_back(saved);else *record=saved;
    _automatic=true;return {};
}
Result<void> CombatSystem::setRelation(std::uint64_t source,std::uint64_t target,Relation relation) {
    if(_settling || _delivering)return fail(Error::Reentrant,"relation change during settlement/delivery");
    return _relations.set(source,target,relation);
}
Result<ActionResult> CombatSystem::damage(ayt::entity::EntityHandle source,ayt::entity::EntityHandle target,ayt::stats::Value amount,std::string_view recipe) {
    return act(Operation::Damage,source,target,amount,recipe);
}
Result<ActionResult> CombatSystem::heal(ayt::entity::EntityHandle source,ayt::entity::EntityHandle target,ayt::stats::Value amount,std::string_view recipe) {
    return act(Operation::Heal,source,target,amount,recipe);
}
Result<AttributeResult> CombatSystem::changeAttribute(ayt::entity::EntityHandle target,std::string_view stat,
    ayt::stats::AttributeOp operation,ayt::stats::Value amount,std::string_view recipe,std::optional<ayt::entity::EntityHandle> source) {
    if(_settling || _delivering)return fail(Error::Reentrant,"attribute change during combat settlement/delivery");
    if(!_stats)return fail(Error::Missing,"combat is not installed");
    auto result=_stats->change(target,stat,operation,amount,recipe,source);
    if(!result)return std::unexpected(Diagnostic{result.error().code==ayt::stats::Error::Reentrant?Error::Reentrant:
        (result.error().code==ayt::stats::Error::Stale?Error::Stale:(result.error().code==ayt::stats::Error::Limit?Error::Limit:Error::NumericFailure)),result.error().message,result.error().path});
    const auto& change=result->changes.front();return AttributeResult{change.before,change.after,change.actual,change.excess};
}
Result<ActionResult> CombatSystem::act(Operation operation,ayt::entity::EntityHandle sourceHandle,ayt::entity::EntityHandle targetHandle,
    ayt::stats::Value amount,std::string_view recipe) {
    if(_settling || _delivering)return fail(Error::Reentrant,"combat action during settlement/delivery");
    if(!_world || !_stats)return fail(Error::Missing,"combat is not installed");
    auto target=bindCombatant(*_world,targetHandle);if(!target)return std::unexpected(target.error());
    auto source=bindCombatant(*_world,sourceHandle);if(!source)return std::unexpected(source.error());
    auto current=resolve(*_world,*target);if(!current)return std::unexpected(current.error());
    auto actor=resolve(*_world,*source);if(!actor)return std::unexpected(actor.error());
    if(!current->combat->life())return fail(Error::Missing,"target life resource is not configured");
    const auto resource=current->combat->life()->policy().resource;
    const auto op=operation==Operation::Damage?ayt::stats::AttributeOp::Decrease:ayt::stats::AttributeOp::Increase;
    auto prepared=_stats->prepareCalculation(targetHandle,resource,op,amount,recipe,sourceHandle);
    if(!prepared)return std::unexpected(Diagnostic{Error::NumericFailure,prepared.error().message,prepared.error().path});
    if(prepared->context.tick==UINT64_MAX)return fail(Error::Limit,"one-shot tick exhausted");
    auto next=_hits.nextAttackId(_token);if(!next)return std::unexpected(next.error());
    AttackRef attack{_token,*next};
    auto opened=_hits.open(attack,{},prepared->context.tick,prepared->context.tick+1);if(!opened)return std::unexpected(opened.error());
    struct Close{HitLedger& hits;AttackRef attack;~Close(){if(!hits.close(attack))std::terminate();}} close{_hits,attack};
    SettlementRequest request;
    request.hit={attack,1,target->ref,prepared->context.tick};
    request.source.instigator=source->ref;request.source.directSource=source->ref;
    request.source.team=actor->combat->team;request.source.attribution=source->ref.object;
    request.filter={operation,operation==Operation::Damage?mask(AllowedTarget::Hostile):(mask(AllowedTarget::Self)|mask(AllowedTarget::Friendly))};
    request.resource=resource;request.expectedVersion=current->stats->version();request.calculation=prepared->context;
    auto result=submit(*target,std::move(request),*prepared->plan,prepared->inputs);
    if(!result)return std::unexpected(result.error());return gameplayResult(*result);
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
    return system->bind(world,*stats);
}
Result<void> registerCombatGameplayComponents(ayt::entity::ComponentRegistry& registry) {
    if(!registry.find<ayt::stats::StatsComponent>()){
        auto result=ayt::stats::registerStatsComponent(registry);
        if(!result)return std::unexpected(Diagnostic{Error::Invalid,result.error().message,result.error().path});
    }
    if(!registry.find<CombatComponent>())return registerCombatComponent(registry);
    return {};
}
Result<CombatSystem*> installCombat(ayt::entity::World& world) {
    auto stats=ayt::stats::registerStatsSystem(world);
    if(!stats)return std::unexpected(Diagnostic{Error::Invalid,stats.error().message,stats.error().path});
    auto combat=registerCombatSystem(world);if(!combat)return std::unexpected(combat.error());
    auto* system=dynamic_cast<CombatSystem*>(world.findSystemByName("CombatSystem"));system->automaticDelivery(true);return system;
}
CombatIntegrationModule::CombatIntegrationModule():_descriptor{.id="AYCombat.Integration",.displayName="Combat gameplay",.version="0.1.0",
    .dependencies={ayt::module::ModuleDependency::required(std::string(ayt::entity::kEntityRuntimeModuleId))}}{}
ayt::module::ModuleResult CombatIntegrationModule::registerTypes(ayt::module::IModuleContext& context) {
    auto* registry=context.findServiceAs<ayt::entity::ComponentRegistry>(ayt::entity::kComponentRegistryModuleService);
    if(!registry)return ayt::module::ModuleResult::failure(ayt::module::ModuleErrorCode::TypeRegistrationFailed,"Combat ComponentRegistry unavailable");
    auto result=registerCombatGameplayComponents(*registry);
    return result?ayt::module::ModuleResult::success():ayt::module::ModuleResult::failure(ayt::module::ModuleErrorCode::TypeRegistrationFailed,result.error().message);
}
ayt::module::ModuleResult CombatIntegrationModule::install(ayt::module::IModuleContext&) {
    auto result=installCombat(ayt::entity::World::instance());
    return result?ayt::module::ModuleResult::success():ayt::module::ModuleResult::failure(ayt::module::ModuleErrorCode::TypeRegistrationFailed,result.error().message);
}
} // namespace ayt::combat
