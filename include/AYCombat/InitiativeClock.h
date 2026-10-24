#pragma once

#include <AYCombat/Combat.h>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace ayt::combat {

/// A game-owned fixed-tick action order. The host decides when one tick passes,
/// which actions are legal, and what Combat/Ability settlement an action uses.
class InitiativeClock {
public:
    enum class Phase : std::uint8_t { Charging, Ready, Acting };
    struct Actor {
        std::uint64_t id = 0;
        std::uint64_t rate = 0;       // Gauge units gained per host tick.
        std::uint64_t gauge = 0;
        std::uint64_t readyTick = 0;  // Earlier readiness wins, then lower ID.
        Phase phase = Phase::Charging;
        bool operator==(const Actor&) const = default;
    };
    struct Snapshot {
        std::uint32_t version = 1;
        std::uint64_t threshold = 0;
        std::uint64_t tick = 0;
        std::vector<Actor> actors;
    };

    /// threshold is 1..1e9. Equal rates of threshold give a round-style queue;
    /// differing rates give an ATB-like queue. At most 256 actors are admitted.
    [[nodiscard]] static Result<InitiativeClock> create(std::uint64_t threshold);
    [[nodiscard]] Result<void> addActor(std::uint64_t id, std::uint64_t rate);
    [[nodiscard]] Result<void> removeActor(std::uint64_t id);
    [[nodiscard]] Result<void> setRate(std::uint64_t id, std::uint64_t rate);
    /// Advance logical time without iterating each intermediate tick. Ready
    /// actors wait in their original order; an acting actor does not recharge.
    [[nodiscard]] Result<void> advance(std::uint64_t ticks);
    /// Start the oldest ready actor. Empty means no actor is available or an
    /// action is already active. Only one action owns the clock at a time.
    [[nodiscard]] std::optional<std::uint64_t> beginNext() noexcept;
    [[nodiscard]] Result<void> finish(std::uint64_t id);
    [[nodiscard]] std::optional<Actor> actor(std::uint64_t id) const noexcept;
    [[nodiscard]] Snapshot snapshot() const;
    /// Validate a complete candidate before replacing live order or progress.
    [[nodiscard]] Result<void> restore(const Snapshot& snapshot);
    [[nodiscard]] std::uint64_t tick() const noexcept { return _tick; }
    [[nodiscard]] std::uint64_t threshold() const noexcept { return _threshold; }

private:
    explicit InitiativeClock(std::uint64_t threshold) : _threshold(threshold) {}
    std::uint64_t _threshold;
    std::uint64_t _tick = 0;
    std::map<std::uint64_t, Actor> _actors;
};

} // namespace ayt::combat
