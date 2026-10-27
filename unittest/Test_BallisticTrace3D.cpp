#include <AYCombat/BallisticTrace3D.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace ayt;
using namespace ayt::combat;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

physics::RaycastHit surface(physics::BodyHandle body,
    physics::ColliderHandle collider, float distance,
    math::FVector3 point, math::FVector3 normal) {
    physics::RaycastHit result;
    result.hit = true;
    result.body = body;
    result.collider = collider;
    result.distance = distance;
    result.point = point;
    result.normal = normal;
    return result;
}

struct Query final : physics::IPhysicsQuery {
    std::vector<std::vector<physics::RaycastHit>> segments;
    std::vector<physics::RaycastAll3D> requests;
    physics::PhysResult forced = physics::PhysResult::Ok;
    bool isReady() const override { return true; }
    physics::PhysResult raycastSync(const math::Ray&,
        physics::RaycastHit& out, physics::PhysLayerMask) override {
        out = {};
        return physics::PhysResult::Unsupported;
    }
    physics::PhysResult overlapSphereSync(const math::FVector3&, float,
        std::vector<physics::BodyHandle>& out,
        physics::PhysLayerMask) override {
        out.clear();
        return physics::PhysResult::Unsupported;
    }
    physics::PhysResult raycastAll3DSync(const physics::RaycastAll3D& ray,
        std::vector<physics::RaycastHit>& out) override {
        requests.push_back(ray);
        if (forced != physics::PhysResult::Ok) {
            out.clear();
            return forced;
        }
        if (requests.size() > segments.size()) {
            out.clear();
            return physics::PhysResult::Ok;
        }
        out = segments[requests.size() - 1];
        return physics::PhysResult::Ok;
    }
};

void run() {
    Query query;
    query.segments = {{
        surface(11, 101, 2, {0, 0, 2}, {0, 0, -1}),
        surface(12, 102, 4, {0, 0, 4}, {0, 0, -1}),
        surface(13, 103, 6, {0, 0, 6}, {0, 0, -1})}};
    BallisticTraceRequest3D request;
    request.origin = {0, 0, 0};
    request.direction = {0, 0, 5}; // normalization is part of the contract
    request.range = 10;
    request.energy = 25;
    const auto rules = [](const physics::RaycastHit& hit)
        -> std::optional<BallisticSurfaceRule3D> {
        return BallisticSurfaceRule3D{hit.body == 11 ? 10.0f : 20.0f,
                                      0.0f, 1.0f};
    };
    auto trace = traceBallisticSurfaces3D(query, request, rules);
    check(trace && trace->queries == 1 && trace->steps.size() == 2,
        "ordered surface trace stops before later objects");
    check(trace->steps[0].decision == BallisticDecision3D::Penetrated
        && trace->steps[0].energyAfter == 15
        && trace->steps[1].decision == BallisticDecision3D::Stopped
        && trace->terminal == BallisticTerminal3D::Stopped
        && trace->finalEnergy == 0,
        "authored surface costs consume the energy budget");
    check(query.requests[0].displacement.z == 10,
        "query uses normalized direction and finite range");

    Query ricochetQuery;
    ricochetQuery.segments = {{
        surface(21, 201, 2, {1.4142135f, 0, 1.4142135f}, {0, 0, -1})},
        {surface(22, 202, 3, {3.536f, 0, -0.708f}, {0, 0, 1})}};
    request.direction = {1, 0, 1};
    request.range = 12;
    request.energy = 100;
    request.maxRicochets = 1;
    auto bounced = traceBallisticSurfaces3D(ricochetQuery, request,
        [](const physics::RaycastHit& hit)
            -> std::optional<BallisticSurfaceRule3D> {
            return hit.body == 21
                ? BallisticSurfaceRule3D{90, 0.8f, 0.5f}
                : BallisticSurfaceRule3D{5, 0, 1};
        });
    check(bounced && bounced->queries == 2 && bounced->steps.size() == 2
        && bounced->steps[0].decision == BallisticDecision3D::Ricochet
        && bounced->steps[1].decision == BallisticDecision3D::Penetrated
        && std::abs(bounced->finalEnergy - 45) < 0.001f,
        "ricochet makes a second segment with retained energy");
    check(ricochetQuery.requests[1].displacement.z < 0
        && ricochetQuery.requests[1].ignoredBodies.size() == 1
        && ricochetQuery.requests[1].ignoredBodies[0] == 21,
        "bounce reflects and excludes only the last surface body");
    request.maxRicochets = 0;
    Query cappedQuery;
    cappedQuery.segments = {ricochetQuery.segments.front()};
    auto capped = traceBallisticSurfaces3D(cappedQuery, request,
        [](const physics::RaycastHit&) -> std::optional<BallisticSurfaceRule3D> {
            return BallisticSurfaceRule3D{90, 0.8f, 0.5f};
        });
    check(capped && capped->terminal == BallisticTerminal3D::RicochetLimit
        && capped->queries == 1, "ricochet limit is explicit");

    Query invalidQuery;
    invalidQuery.segments = {query.segments.front()};
    auto invalid = traceBallisticSurfaces3D(invalidQuery, request,
        [](const physics::RaycastHit&) -> std::optional<BallisticSurfaceRule3D> {
            return BallisticSurfaceRule3D{-1, 0, 1};
        });
    check(!invalid && invalid.error() == BallisticTraceError3D::RuleFailed,
        "invalid material rejects whole trace");
    Query limitQuery;
    limitQuery.forced = physics::PhysResult::OutOfRange;
    auto limited = traceBallisticSurfaces3D(limitQuery, request, rules);
    check(!limited && limited.error() == BallisticTraceError3D::HitLimit,
        "truncated geometry is never accepted as a complete trace");
    request.ignoredBodies = {11, 11};
    Query neverQueried;
    auto bad = traceBallisticSurfaces3D(neverQueried, request, rules);
    check(!bad && bad.error() == BallisticTraceError3D::Invalid
        && neverQueried.requests.empty(), "invalid requests fail before query");
}
}

int main() {
    try {
        run();
        std::cout << "AYCombat: 3D ballistic surface trace passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "AYCombat: 3D ballistic surface trace failed: "
                  << ex.what() << '\n';
        return 1;
    }
}
