#pragma once

#include <AYPhysics/IPhysicsQuery.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <vector>

namespace ayt::combat {

/// Per-surface gameplay policy selected by the game from a collider/material.
/// energyCost is a fixed authored cost for this surface, not inferred object
/// thickness. A front-facing hit ricochets when incidence cosine is below
/// ricochetBelowCosine; the remaining energy is multiplied by retention.
struct BallisticSurfaceRule3D {
    float energyCost = 0;
    float ricochetBelowCosine = 0;
    float ricochetRetention = 1;
};

struct BallisticTraceRequest3D {
    math::FVector3 origin{}, direction{0, 0, 1};
    float range = 0;
    float energy = 0;
    physics::PhysLayerMask layerMask = 0xFFFFFFFFu;
    std::vector<physics::BodyHandle> ignoredBodies;
    std::uint32_t maxHitsPerSegment = 64;
    std::uint32_t maxRicochets = 0;
};

enum class BallisticTraceError3D : std::uint8_t {
    Invalid, QueryUnavailable, QueryFailed, HitLimit, RuleMissing, RuleFailed
};
enum class BallisticDecision3D : std::uint8_t {
    Penetrated, Stopped, Ricochet
};
enum class BallisticTerminal3D : std::uint8_t {
    Clear, Stopped, RicochetLimit, RangeExhausted
};

struct BallisticSurfaceStep3D {
    physics::RaycastHit hit;
    std::uint32_t segment = 0;
    float energyBefore = 0, energyAfter = 0;
    float incidenceCosine = 0;
    BallisticDecision3D decision = BallisticDecision3D::Stopped;
    math::FVector3 outgoingDirection{};
};

struct BallisticTrace3D {
    std::vector<BallisticSurfaceStep3D> steps;
    BallisticTerminal3D terminal = BallisticTerminal3D::Clear;
    float finalEnergy = 0;
    std::uint32_t queries = 0;
};

using BallisticSurfaceRuleResolver3D = std::function<
    std::optional<BallisticSurfaceRule3D>(const physics::RaycastHit&)>;

/// Query a finite ordered 3D ray, apply authored per-surface costs, and issue
/// at most four further queries for ricochets. Failure returns no partial trace.
/// Each synchronous query consumes the Physics query budget. This does not
/// infer exit thickness, projectile shape, damage, armor, or hit authority;
/// the game maps accepted steps to Combat/Stats at its serialized Sim boundary.
[[nodiscard]] std::expected<BallisticTrace3D, BallisticTraceError3D>
traceBallisticSurfaces3D(physics::IPhysicsQuery& query,
    const BallisticTraceRequest3D& request,
    const BallisticSurfaceRuleResolver3D& resolve);

} // namespace ayt::combat
