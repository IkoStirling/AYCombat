#pragma once
#include <AYCombat/EntityIntegration.h>
#include <AYPhysics/PhysicsManager.h>
#include <array>

namespace ayt::combat {
/// Source-following XY overlap. Physics only supplies geometry; Combat owns membership.
struct AreaQuery2D {
    float radius=0.0f;
    std::array<float,2> offset{};
    ayt::physics::PhysLayerMask layerMask=0xffffffffu;
    bool includeSource=false;
};
/// Optional pure selection fact, evaluated for each in-range Entity every sweep.
/// Do not mutate World or Combat from this callback; captured state must outlive the area.
using AreaSelection2D=std::function<bool(ayt::entity::EntityHandle)>;
Result<void> validateAreaQuery2D(const AreaQuery2D& query);
/// Build one complete Box2D candidate provider for CombatSystem::startArea.
/// Only overlapping entities with Stats and Combat components count as candidates.
/// World and PhysicsManager must outlive the area; the source needs a Transform.
Result<AreaCandidateProvider> makeAreaCandidateProvider2D(ayt::entity::World& world,
    ayt::physics::PhysicsManager& physics,ayt::entity::EntityHandle source,
    AreaQuery2D query,AreaSelection2D selection={});
} // namespace ayt::combat
