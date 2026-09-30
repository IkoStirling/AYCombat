#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ayt::combat {

enum class Error { Invalid, Missing, Duplicate, Stale, Limit, Pending, AlreadyHit, Closed, NumericFailure, Reentrant };
struct Diagnostic { Error code; std::string message; std::string path; };
template<class T> using Result = std::expected<T, Diagnostic>;

/// Host-resolved identity. Core stores values, never Entity/World pointers.
struct CombatantRef {
    std::uint64_t domain = 0, object = 0, generation = 0;
    bool valid() const noexcept { return domain && object && generation; }
    auto operator<=>(const CombatantRef&) const = default;
};
enum class SourceKind { Actor, Environment };
/// Owned provenance snapshot; retained identities do not keep source entities alive.
/// directSource may identify the action provider (such as equipment), while instigator
/// identifies the actor used for self/relation permission. Identity values are host-owned.
struct SourceContext {
    SourceKind kind = SourceKind::Actor;
    std::optional<CombatantRef> instigator;
    std::optional<CombatantRef> directSource;
    std::uint64_t attribution = 0, team = 0, ability = 0, effect = 0;
    /// Ability owner-assigned release instance; scoped by the source domain.
    std::uint64_t abilityInstance = 0;
};
Result<void> validateSource(const SourceContext& source);

enum class Operation { Damage, Heal };
enum class Capability : std::uint32_t { Damageable = 1, Healable = 2 };
struct CombatantView {
    CombatantRef ref;
    std::uint64_t team = 0;
    std::uint32_t capabilities = 0;
    bool active = true;
};
enum class Relation { Unknown, Friendly, Neutral, Hostile };
/// Explicit directional relations. Different teams are not implicitly hostile.
class RelationTable {
public:
    explicit RelationTable(bool sameTeamFriendly = false) : _sameTeamFriendly(sameTeamFriendly) {}
    Result<void> set(std::uint64_t observerTeam, std::uint64_t targetTeam, Relation relation);
    Relation query(std::uint64_t observerTeam, std::uint64_t targetTeam) const noexcept;
    std::uint64_t version() const noexcept { return _version; }
private:
    struct Entry { std::uint64_t observer, target; Relation relation; };
    bool _sameTeamFriendly;
    std::uint64_t _version = 1;
    std::vector<Entry> _entries;
};
enum class AllowedTarget : std::uint32_t { Self = 1, Friendly = 2, Neutral = 4, Hostile = 8, Unknown = 16, Environment = 32 };
constexpr std::uint32_t mask(AllowedTarget target) noexcept { return static_cast<std::uint32_t>(target); }
constexpr std::uint32_t mask(Capability capability) noexcept { return static_cast<std::uint32_t>(capability); }
struct TargetFilter { Operation operation = Operation::Damage; std::uint32_t allowed = mask(AllowedTarget::Hostile); };
enum class Denial { None, Inactive, MissingCapability, SelfForbidden, UnknownRelation, RelationForbidden, EnvironmentForbidden };
struct TargetDecision { bool allowed = false; Denial denial = Denial::None; Relation relation = Relation::Unknown; };
/// Pure permission query. Caller supplies a current target view and chosen source snapshot.
Result<TargetDecision> filterTarget(const SourceContext& source, const CombatantView& target,
                                    const RelationTable& relations, TargetFilter filter);

/// Candidates are already mapped by the host; Core never casts physics userData.
struct HitCandidate { CombatantRef target; std::uint64_t collider = 0; std::uint32_t part = 0; std::int32_t priority = 0; };
enum class CandidateScope { Target, Part };
/// Stable selection: target/part, highest priority, lowest collider/part on ties.
Result<std::vector<HitCandidate>> selectCandidates(std::span<const HitCandidate> candidates, CandidateScope scope);

/// Producer sequence is assigned by the simulation owner, not worker completion order.
struct RequestOrder {
    std::uint64_t tick = 0, producer = 0, sequence = 0, request = 0;
    auto operator<=>(const RequestOrder&) const = default;
};

} // namespace ayt::combat
