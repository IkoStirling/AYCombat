#include <AYCombat/InitiativeClock.h>

#include <limits>

namespace ayt::combat {
namespace {
template<class T> Result<T> fail(Error code, const char* message) {
    return std::unexpected(Diagnostic{code, message, "initiative-clock"});
}
}

Result<InitiativeClock> InitiativeClock::create(std::uint64_t threshold) {
    if (!threshold || threshold > 1'000'000'000)
        return fail<InitiativeClock>(Error::Invalid, "Invalid initiative threshold");
    return InitiativeClock(threshold);
}

Result<void> InitiativeClock::addActor(std::uint64_t id, std::uint64_t rate) {
    if (!id || !rate) return fail<void>(Error::Invalid, "Actor ID and rate must be nonzero");
    if (_actors.contains(id)) return fail<void>(Error::Duplicate, "Actor already registered");
    if (_actors.size() >= 256) return fail<void>(Error::Limit, "Initiative actor budget exceeded");
    _actors.emplace(id, Actor{.id=id, .rate=rate});
    return {};
}

Result<void> InitiativeClock::removeActor(std::uint64_t id) {
    if (!_actors.erase(id)) return fail<void>(Error::Missing, "Actor is not registered");
    return {};
}

Result<void> InitiativeClock::setRate(std::uint64_t id, std::uint64_t rate) {
    if (!rate) return fail<void>(Error::Invalid, "Rate must be nonzero");
    const auto it = _actors.find(id);
    if (it == _actors.end()) return fail<void>(Error::Missing, "Actor is not registered");
    it->second.rate = rate;
    return {};
}

Result<void> InitiativeClock::advance(std::uint64_t ticks) {
    if (ticks > std::numeric_limits<std::uint64_t>::max() - _tick)
        return fail<void>(Error::Limit, "Initiative tick overflow");
    const auto nextTick = _tick + ticks;
    for (auto& [id, actor] : _actors) {
        (void)id;
        if (actor.phase != Phase::Charging) continue;
        const auto remaining = _threshold - actor.gauge;
        const auto needed = remaining / actor.rate + (remaining % actor.rate != 0);
        if (needed <= ticks) {
            actor.gauge = _threshold;
            actor.readyTick = _tick + needed;
            actor.phase = Phase::Ready;
        } else {
            // needed > ticks proves rate*ticks < remaining <= threshold.
            actor.gauge += actor.rate * ticks;
        }
    }
    _tick = nextTick;
    return {};
}

std::optional<std::uint64_t> InitiativeClock::beginNext() noexcept {
    for (const auto& [id, actor] : _actors)
        if (actor.phase == Phase::Acting) return std::nullopt;
    Actor* chosen = nullptr;
    for (auto& [id, actor] : _actors) {
        (void)id;
        if (actor.phase != Phase::Ready) continue;
        if (!chosen || actor.readyTick < chosen->readyTick
            || (actor.readyTick == chosen->readyTick && actor.id < chosen->id))
            chosen = &actor;
    }
    if (!chosen) return std::nullopt;
    const auto id = chosen->id;
    chosen->phase = Phase::Acting;
    chosen->gauge = 0;
    chosen->readyTick = 0;
    return id;
}

Result<void> InitiativeClock::finish(std::uint64_t id) {
    const auto it = _actors.find(id);
    if (it == _actors.end()) return fail<void>(Error::Missing, "Actor is not registered");
    if (it->second.phase != Phase::Acting)
        return fail<void>(Error::Stale, "Actor does not own the active action");
    it->second.phase = Phase::Charging;
    return {};
}

std::optional<InitiativeClock::Actor> InitiativeClock::actor(
    std::uint64_t id) const noexcept {
    const auto it = _actors.find(id);
    return it == _actors.end() ? std::nullopt : std::optional{it->second};
}

InitiativeClock::Snapshot InitiativeClock::snapshot() const {
    Snapshot result{.threshold=_threshold, .tick=_tick};
    result.actors.reserve(_actors.size());
    for (const auto& [id, actor] : _actors) {
        (void)id;
        result.actors.push_back(actor);
    }
    return result;
}

Result<void> InitiativeClock::restore(const Snapshot& snapshot) {
    if (snapshot.version != 1 || snapshot.threshold != _threshold
        || snapshot.actors.size() > 256)
        return fail<void>(Error::Invalid, "Invalid initiative snapshot header");
    std::map<std::uint64_t, Actor> staged;
    std::uint64_t previousId = 0;
    bool active = false;
    for (const auto& actor : snapshot.actors) {
        if (!actor.id || actor.id <= previousId || !actor.rate)
            return fail<void>(Error::Invalid, "Invalid or unordered initiative actor");
        previousId = actor.id;
        switch (actor.phase) {
        case Phase::Charging:
            if (actor.gauge >= _threshold || actor.readyTick)
                return fail<void>(Error::Invalid, "Invalid charging actor");
            break;
        case Phase::Ready:
            if (actor.gauge != _threshold || !actor.readyTick
                || actor.readyTick > snapshot.tick)
                return fail<void>(Error::Invalid, "Invalid ready actor");
            break;
        case Phase::Acting:
            if (active || !snapshot.tick || actor.gauge || actor.readyTick)
                return fail<void>(Error::Invalid, "Invalid active action");
            active = true;
            break;
        default:
            return fail<void>(Error::Invalid, "Unknown initiative phase");
        }
        staged.emplace(actor.id, actor);
    }
    _actors.swap(staged);
    _tick = snapshot.tick;
    return {};
}

} // namespace ayt::combat
