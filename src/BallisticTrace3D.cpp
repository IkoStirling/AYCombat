#include <AYCombat/BallisticTrace3D.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace ayt::combat {
namespace {
bool finite(const math::FVector3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y)
        && std::isfinite(v.z);
}
float dot(const math::FVector3& a, const math::FVector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
math::FVector3 scaled(const math::FVector3& v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}
math::FVector3 added(const math::FVector3& a, const math::FVector3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
std::optional<math::FVector3> unit(const math::FVector3& v) {
    if (!finite(v)) return {};
    const float squared = dot(v, v);
    if (!std::isfinite(squared) || squared <= 0) return {};
    const auto normalized = scaled(v, 1.0f / std::sqrt(squared));
    return finite(normalized) ? std::optional<math::FVector3>{normalized}
                              : std::nullopt;
}
bool validRule(const BallisticSurfaceRule3D& rule) {
    return std::isfinite(rule.energyCost) && rule.energyCost >= 0
        && std::isfinite(rule.ricochetBelowCosine)
        && rule.ricochetBelowCosine >= 0
        && rule.ricochetBelowCosine <= 1
        && std::isfinite(rule.ricochetRetention)
        && rule.ricochetRetention > 0 && rule.ricochetRetention <= 1;
}
} // namespace

std::expected<BallisticTrace3D, BallisticTraceError3D>
traceBallisticSurfaces3D(physics::IPhysicsQuery& query,
    const BallisticTraceRequest3D& request,
    const BallisticSurfaceRuleResolver3D& resolve) {
    const auto direction = unit(request.direction);
    if (!direction || !finite(request.origin) || !std::isfinite(request.range)
        || request.range <= 0 || request.range > 10000
        || !std::isfinite(request.energy) || request.energy <= 0
        || !resolve || request.maxHitsPerSegment == 0
        || request.maxHitsPerSegment > 64 || request.maxRicochets > 4
        || request.ignoredBodies.size() > 64)
        return std::unexpected(BallisticTraceError3D::Invalid);
    std::set<physics::BodyHandle> ignored;
    for (const auto body : request.ignoredBodies)
        if (body == physics::InvalidBodyHandle || !ignored.insert(body).second)
            return std::unexpected(BallisticTraceError3D::Invalid);
    if (!query.isReady())
        return std::unexpected(BallisticTraceError3D::QueryUnavailable);

    BallisticTrace3D result;
    result.finalEnergy = request.energy;
    auto origin = request.origin;
    auto travelDirection = *direction;
    float remaining = request.range;
    physics::BodyHandle priorRicochetBody = physics::InvalidBodyHandle;
    constexpr float epsilon = 0.001f;
    for (std::uint32_t segment = 0; segment <= request.maxRicochets;
             ++segment) {
        try {
            physics::RaycastAll3D ray;
            ray.origin = origin;
            ray.displacement = scaled(travelDirection, remaining);
            ray.layerMask = request.layerMask;
            ray.ignoredBodies = request.ignoredBodies;
            if (priorRicochetBody != physics::InvalidBodyHandle
                && std::find(ray.ignoredBodies.begin(), ray.ignoredBodies.end(),
                    priorRicochetBody) == ray.ignoredBodies.end()) {
                if (ray.ignoredBodies.size() == 64)
                    return std::unexpected(BallisticTraceError3D::HitLimit);
                ray.ignoredBodies.push_back(priorRicochetBody);
            }
            ray.maxHits = request.maxHitsPerSegment;
            std::vector<physics::RaycastHit> hits;
            physics::PhysResult status;
            try { status = query.raycastAll3DSync(ray, hits); }
            catch (...) {
                return std::unexpected(BallisticTraceError3D::QueryFailed);
            }
            ++result.queries;
            if (status == physics::PhysResult::Unsupported)
                return std::unexpected(BallisticTraceError3D::QueryUnavailable);
            if (status == physics::PhysResult::OutOfRange)
                return std::unexpected(BallisticTraceError3D::HitLimit);
            if (status != physics::PhysResult::Ok)
                return std::unexpected(BallisticTraceError3D::QueryFailed);
            if (hits.size() > request.maxHitsPerSegment)
                return std::unexpected(BallisticTraceError3D::HitLimit);
            if (hits.empty()) return result;
            float previousDistance = -1;
            bool bounced = false;
            for (const auto& hit : hits) {
                if (result.steps.size() == 64)
                    return std::unexpected(BallisticTraceError3D::HitLimit);
                if (!hit.hit || hit.body == physics::InvalidBodyHandle
                    || hit.collider == physics::InvalidColliderHandle
                    || !finite(hit.point) || !std::isfinite(hit.distance)
                    || hit.distance < 0 || hit.distance > remaining
                    || hit.distance < previousDistance)
                    return std::unexpected(BallisticTraceError3D::QueryFailed);
                previousDistance = hit.distance;
                const auto normal = unit(hit.normal);
                if (!normal) return std::unexpected(BallisticTraceError3D::QueryFailed);
                std::optional<BallisticSurfaceRule3D> rule;
                try { rule = resolve(hit); }
                catch (...) {
                    return std::unexpected(BallisticTraceError3D::RuleFailed);
                }
                if (!rule) return std::unexpected(BallisticTraceError3D::RuleMissing);
                if (!validRule(*rule))
                    return std::unexpected(BallisticTraceError3D::RuleFailed);
                BallisticSurfaceStep3D step;
                step.hit = hit;
                step.segment = segment;
                step.energyBefore = result.finalEnergy;
                const float facingDot = dot(travelDirection, *normal);
                step.incidenceCosine = std::clamp(-facingDot, 0.0f, 1.0f);
                if (facingDot < 0 && rule->ricochetBelowCosine > 0
                    && step.incidenceCosine < rule->ricochetBelowCosine) {
                    const auto reflected = unit(added(travelDirection,
                        scaled(*normal, -2.0f * facingDot)));
                    if (!reflected)
                        return std::unexpected(BallisticTraceError3D::RuleFailed);
                    step.decision = BallisticDecision3D::Ricochet;
                    step.energyAfter = result.finalEnergy
                        * rule->ricochetRetention;
                    step.outgoingDirection = *reflected;
                    result.finalEnergy = step.energyAfter;
                    result.steps.push_back(step);
                    if (segment == request.maxRicochets) {
                        result.terminal = BallisticTerminal3D::RicochetLimit;
                        return result;
                    }
                    remaining -= hit.distance + epsilon;
                    if (remaining <= 0) {
                        result.terminal = BallisticTerminal3D::RangeExhausted;
                        return result;
                    }
                    origin = added(hit.point, scaled(*reflected, epsilon));
                    travelDirection = *reflected;
                    priorRicochetBody = hit.body;
                    bounced = true;
                    break;
                }
                if (result.finalEnergy <= rule->energyCost) {
                    step.decision = BallisticDecision3D::Stopped;
                    step.energyAfter = 0;
                    result.finalEnergy = 0;
                    result.steps.push_back(step);
                    result.terminal = BallisticTerminal3D::Stopped;
                    return result;
                }
                result.finalEnergy -= rule->energyCost;
                step.decision = BallisticDecision3D::Penetrated;
                step.energyAfter = result.finalEnergy;
                step.outgoingDirection = travelDirection;
                result.steps.push_back(step);
            }
            if (!bounced) return result;
        } catch (...) {
            return std::unexpected(BallisticTraceError3D::QueryFailed);
        }
    }
    return result;
}

} // namespace ayt::combat
