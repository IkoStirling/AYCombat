#pragma once
#include <AYCombat/Combat.h>
#include <utility>

namespace ayt::combat {

struct AttackRef {
    std::uint64_t domain = 0, id = 0;
    bool valid() const noexcept { return domain && id; }
    auto operator<=>(const AttackRef&) const = default;
};
enum class HitScope { Target, Part, Segment, Periodic };
struct HitPolicy { HitScope scope = HitScope::Target; std::uint64_t interval = 0; };
struct HitRequest {
    AttackRef attack;
    std::uint64_t request = 0;
    CombatantRef target;
    std::uint64_t tick = 0;
    std::uint32_t part = 0, segment = 0;
};
struct Reservation { AttackRef attack; std::uint64_t request = 0, attempt = 0; };
struct LedgerLimits { std::size_t domains = 64, attacks = 1024, requestsPerAttack = 4096; };

/// Portable owner snapshot. Completed requests and retired attack high-water
/// marks must be saved together with Combat/Action state in one game slot.
/// Pending reservations are never durable; snapshot() refuses them.
struct HitLedgerRecordState {
    std::uint64_t request = 0, tick = 0, attempt = 0;
    CombatantRef target;
    std::uint32_t part = 0, segment = 0;
    bool completed = false;
};
struct HitLedgerAttackState {
    AttackRef ref;
    HitPolicy policy;
    std::uint64_t start = 0, end = 0, lastTick = 0;
    std::vector<HitLedgerRecordState> records;
};
struct HitLedgerState {
    LedgerLimits limits;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> domainHighWater;
    std::vector<HitLedgerAttackState> attacks;
};

/// Single simulation owner. Completed request identities survive until attack close.
/// Attack IDs increase within each domain; closing never makes an old attack reusable.
class HitLedger {
public:
    explicit HitLedger(LedgerLimits limits = {}) : _limits(limits) {}
    HitLedger(const HitLedger&) = delete;
    HitLedger& operator=(const HitLedger&) = delete;
    HitLedger(HitLedger&&) noexcept = default;
    HitLedger& operator=(HitLedger&&) noexcept = default;
    /// Half-open attack window [start,end), with a validated periodic interval.
    Result<void> open(AttackRef attack, HitPolicy policy, std::uint64_t start, std::uint64_t end);
    /// Reserve before numerical work; duplicate pending/completed requests are rejected.
    Result<Reservation> reserve(const HitRequest& request);
    /// Valid pending completion is allocation-free. No callbacks may mutate this ledger.
    bool complete(Reservation reservation) noexcept;
    /// Failed work releases the hit allowance but retains request/key identity for retry.
    bool release(Reservation reservation) noexcept;
    /// Pending work must be completed/released before close. Late closed requests reject.
    Result<void> close(AttackRef attack);
    /// Owner's next one-shot identity, compatible with explicitly opened advanced attacks.
    Result<std::uint64_t> nextAttackId(std::uint64_t domain) const;
    /// Copy an unpublished candidate for an owner-controlled atomic batch.
    /// The copy has no shared mutable storage with the live ledger.
    [[nodiscard]] HitLedger stagedCopy() const;
    /// Capture only a quiescent ledger. A pending reservation makes this fail
    /// without modifying out. Retryable records retain their attempt counter.
    [[nodiscard]] bool snapshot(HitLedgerState& out) const;
    /// Validate a saved snapshot before replacing this ledger. Failure leaves
    /// the live ledger unchanged; caller owns file format and atomic slot save.
    [[nodiscard]] bool restore(const HitLedgerState& state);
    std::size_t activeAttacks() const noexcept { return _attacks.size(); }
    std::size_t retainedRequests() const noexcept;
private:
    struct Key {
        CombatantRef target;
        std::uint32_t part = 0, segment = 0;
        bool operator==(const Key&) const = default;
    };
    enum class State { Pending, Completed, Retryable };
    struct Record { std::uint64_t request, tick; Key key; State state; std::uint64_t attempt = 1; };
    struct Attack { AttackRef ref; HitPolicy policy; std::uint64_t start, end, lastTick; std::vector<Record> records; };
    struct Domain { std::uint64_t id, highWater; };
    LedgerLimits _limits;
    std::vector<Domain> _domains;
    std::vector<Attack> _attacks;
};

} // namespace ayt::combat
