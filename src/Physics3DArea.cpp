#include <AYCombat/Physics3DArea.h>
#include <AYPhysics/PhysicsWorld3D.h>
#include <AYEntity.h>
#include <AYEntity/components/RigidBodyComponent.h>
#include <AYEntity/components/TransformComponent.h>
#include <cmath>
#include <map>
#include <set>

namespace ayt::combat {
namespace {
template<class T> Result<T> invalid(Error code, std::string message) {
    return std::unexpected(Diagnostic{code, std::move(message), {}});
}
}
Result<void> validateAreaQuery3D(const AreaQuery3D& query) {
    if (!std::isfinite(query.radius) || query.radius <= 0 || query.radius > 1000 ||
        !std::isfinite(query.offset.x) || !std::isfinite(query.offset.y) ||
        !std::isfinite(query.offset.z) || !query.layerMask)
        return invalid<void>(Error::Invalid, "invalid 3D area query");
    return {};
}
Result<AreaCandidateProvider> makeAreaCandidateProvider3D(ayt::entity::World& world,
    ayt::physics::PhysicsManager& physics, ayt::entity::EntityHandle source,
    AreaQuery3D query, AreaSelection3D selection) {
    if (auto valid = validateAreaQuery3D(query); !valid)
        return std::unexpected(valid.error());
    if (!physics.world3D())
        return invalid<AreaCandidateProvider>(Error::Missing, "3D physics world unavailable");
    auto* entity = world.getEntityByHandle(source);
    if (!entity || !entity->getComponent<ayt::entity::Transform>())
        return invalid<AreaCandidateProvider>(Error::Missing, "3D area source Transform missing");
    return AreaCandidateProvider{[&world, &physics, source, query, selection = std::move(selection)]()
        -> Result<std::vector<AreaCandidate>> {
        auto* entity = world.getEntityByHandle(source);
        auto* transform = entity ? entity->getComponent<ayt::entity::Transform>() : nullptr;
        if (!transform) return std::vector<AreaCandidate>{};
        auto* backend = physics.world3D();
        if (!backend)
            return invalid<std::vector<AreaCandidate>>(Error::Stale, "3D physics world unavailable");
        const ayt::math::FVector3 center{transform->position.x + query.offset.x,
            transform->position.y + query.offset.y, transform->position.z + query.offset.z};
        std::vector<ayt::physics::BodyHandle> bodies;
        if (backend->overlapSphereSync(center, query.radius, bodies, query.layerMask) !=
            ayt::physics::PhysResult::Ok)
            return invalid<std::vector<AreaCandidate>>(Error::Invalid, "3D area overlap query failed");
        const std::set<ayt::physics::BodyHandle> overlapping(bodies.begin(), bodies.end());
        std::map<ayt::physics::BodyHandle, ayt::entity::EntityHandle> mapped;
        for (auto* candidate : world.getAllEntities()) {
            auto* rigid = candidate->getComponent<ayt::entity::RigidBodyComponent>();
            if (!rigid || rigid->getPhysicsDimension() !=
                ayt::entity::RigidBodyComponent::PhysicsDimension::ThreeD ||
                rigid->getBodyHandle() == ayt::physics::InvalidBodyHandle ||
                !overlapping.contains(rigid->getBodyHandle()) ||
                !candidate->getComponent<ayt::stats::StatsComponent>() ||
                !candidate->getComponent<CombatComponent>()) continue;
            auto handle = world.getEntityHandle(candidate->getId());
            if (!mapped.emplace(rigid->getBodyHandle(), handle).second)
                return invalid<std::vector<AreaCandidate>>(Error::Invalid,
                    "3D body belongs to multiple entities");
        }
        std::vector<AreaCandidate> candidates;
        candidates.reserve(bodies.size());
        std::set<ayt::physics::BodyHandle> seen;
        for (auto body : bodies) {
            if (!seen.insert(body).second) continue;
            auto found = mapped.find(body);
            if (found == mapped.end() || (!query.includeSource && found->second == source)) continue;
            candidates.push_back({found->second, selection ? selection(found->second) : false});
        }
        return candidates;
    }};
}
} // namespace ayt::combat
