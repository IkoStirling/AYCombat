#include <AYCombat/HitLedger.h>
#include <algorithm>
#include <limits>

namespace ayt::combat {
Result<std::uint64_t> HitLedger::nextAttackId(std::uint64_t domain) const {
    if(!domain)return std::unexpected(Diagnostic{Error::Invalid,"invalid attack domain",{}});
    for(const auto& entry:_domains)if(entry.id==domain) {
        if(entry.highWater==UINT64_MAX)return std::unexpected(Diagnostic{Error::Limit,"attack identities exhausted",{}});
        return entry.highWater+1;
    }
    return 1;
}
namespace {
auto fail(Error code, const char* message) { return std::unexpected(Diagnostic{code, message, {}}); }
}

Result<void> HitLedger::open(AttackRef ref, HitPolicy policy, std::uint64_t start, std::uint64_t end) {
    if (!ref.valid() || start >= end ||
        (policy.scope != HitScope::Target && policy.scope != HitScope::Part &&
         policy.scope != HitScope::Segment && policy.scope != HitScope::Periodic) ||
        (policy.scope == HitScope::Periodic ? !policy.interval : policy.interval != 0))
        return fail(Error::Invalid, "invalid attack window, identity or hit policy");
    if (std::any_of(_attacks.begin(), _attacks.end(), [&](const Attack& a) { return a.ref == ref; }))
        return fail(Error::Duplicate, "attack is already open");
    const auto domain = std::find_if(_domains.begin(), _domains.end(), [&](const Domain& d) { return d.id == ref.domain; });
    if (domain != _domains.end() && ref.id <= domain->highWater)
        return fail(Error::Stale, "attack identity must increase within its domain");
    const bool newDomain = domain == _domains.end();
    if (_attacks.size() >= _limits.attacks || (newDomain && _domains.size() >= _limits.domains))
        return fail(Error::Limit, "attack/domain capacity exhausted");
    const auto index = static_cast<std::size_t>(domain - _domains.begin());
    // Complete allocations before publishing either the cursor or active attack.
    if (newDomain) _domains.reserve(_domains.size() + 1);
    _attacks.reserve(_attacks.size() + 1);
    _attacks.push_back({ref, policy, start, end, start, {}});
    if (newDomain) _domains.push_back({ref.domain, ref.id});
    else _domains[index].highWater = ref.id;
    return {};
}

Result<Reservation> HitLedger::reserve(const HitRequest& request) {
    if (!request.attack.valid() || !request.request || !request.target.valid())
        return fail(Error::Invalid, "invalid request, attack or target identity");
    if (request.target.domain != request.attack.domain)
        return fail(Error::Stale, "target and attack belong to different domains");
    auto attack = std::find_if(_attacks.begin(), _attacks.end(), [&](const Attack& a) { return a.ref == request.attack; });
    if (attack == _attacks.end()) {
        const bool closed = std::any_of(_domains.begin(), _domains.end(), [&](const Domain& d) {
            return d.id == request.attack.domain && request.attack.id <= d.highWater;
        });
        return fail(closed ? Error::Closed : Error::Missing, "attack is not active");
    }
    if (request.tick < attack->start || request.tick >= attack->end || request.tick < attack->lastTick)
        return fail(Error::Stale, "request outside attack window or simulation order");
    Key key{request.target};
    if (attack->policy.scope == HitScope::Part) key.part = request.part;
    if (attack->policy.scope == HitScope::Segment) key.segment = request.segment;
    auto retry = attack->records.end();
    for (auto record = attack->records.begin(); record != attack->records.end(); ++record) {
        if (record->request != request.request) continue;
        if (record->state == State::Completed) return fail(Error::Duplicate, "request already committed");
        if (record->state == State::Pending) return fail(Error::Pending, "request already pending");
        if (record->key != key) return fail(Error::Invalid, "retry cannot change logical hit identity");
        retry = record;
    }
    for (const auto& record : attack->records) {
        if (record.key != key || record.state == State::Retryable) continue;
        if (record.state == State::Pending) return fail(Error::Pending, "logical hit already pending");
        if (attack->policy.scope != HitScope::Periodic || request.tick - record.tick < attack->policy.interval)
            return fail(Error::AlreadyHit, "logical hit allowance already consumed");
    }
    std::uint64_t attempt = 1;
    if (retry != attack->records.end()) {
        if (retry->attempt == std::numeric_limits<std::uint64_t>::max())
            return fail(Error::Limit, "reservation attempt identity exhausted");
        retry->tick = request.tick;
        retry->state = State::Pending;
        ++retry->attempt;
        attempt = retry->attempt;
    } else {
        if (attack->records.size() >= _limits.requestsPerAttack)
            return fail(Error::Limit, "request retention capacity exhausted");
        attack->records.push_back({request.request, request.tick, key, State::Pending});
    }
    attack->lastTick = request.tick;
    return Reservation{request.attack, request.request, attempt};
}

bool HitLedger::complete(Reservation reservation) noexcept {
    for (auto& attack : _attacks) if (attack.ref == reservation.attack)
        for (auto& record : attack.records) if (record.request == reservation.request &&
            record.attempt == reservation.attempt && record.state == State::Pending) {
            record.state = State::Completed;
            return true;
        }
    return false;
}

bool HitLedger::release(Reservation reservation) noexcept {
    for (auto& attack : _attacks) if (attack.ref == reservation.attack)
        for (auto& record : attack.records) if (record.request == reservation.request &&
            record.attempt == reservation.attempt && record.state == State::Pending) {
            record.state = State::Retryable;
            return true;
        }
    return false;
}

Result<void> HitLedger::close(AttackRef ref) {
    if (!ref.valid()) return fail(Error::Invalid, "invalid attack identity");
    auto attack = std::find_if(_attacks.begin(), _attacks.end(), [&](const Attack& a) { return a.ref == ref; });
    if (attack == _attacks.end()) return fail(Error::Closed, "attack is not active");
    if (std::any_of(attack->records.begin(), attack->records.end(), [](const Record& r) { return r.state == State::Pending; }))
        return fail(Error::Pending, "release or complete pending requests before close");
    _attacks.erase(attack);
    return {};
}

std::size_t HitLedger::retainedRequests() const noexcept {
    std::size_t count = 0;
    for (const auto& attack : _attacks) count += attack.records.size();
    return count;
}

} // namespace ayt::combat
