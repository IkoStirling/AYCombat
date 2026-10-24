#pragma once
#include <AYCombat/EntityIntegration.h>
#include <AYPhysics/PhysicsManager.h>
#include <AYMath/MathTypes.h>

namespace ayt::combat {
/// Source-following Jolt sphere query. Physics provides candidates only;
/// Combat applies selection, relation and area membership rules.
struct AreaQuery3D {
    float radius = 0.0f;
    ayt::math::FVector3 offset{};
    ayt::physics::PhysLayerMask layerMask = 0xffffffffu;
    bool includeSource = false;
};
/// Pure selection fact evaluated for each in-range actor on every sweep.
using AreaSelection3D = std::function<bool(ayt::entity::EntityHandle)>;
Result<void> validateAreaQuery3D(const AreaQuery3D& query);
/// Build a complete current candidate provider for CombatSystem::startArea.
/// Only uniquely mapped 3D rigid bodies with Stats and Combat count. The
/// World and PhysicsManager must outlive this provider and its owning area.
Result<AreaCandidateProvider> makeAreaCandidateProvider3D(ayt::entity::World& world,
    ayt::physics::PhysicsManager& physics, ayt::entity::EntityHandle source,
    AreaQuery3D query, AreaSelection3D selection = {});
} // namespace ayt::combat
