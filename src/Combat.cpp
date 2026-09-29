#include <AYCombat/Combat.h>
#include <algorithm>
#include <limits>
#include <tuple>

namespace ayt::combat {
namespace {
auto fail(Error code, const char* message) { return std::unexpected(Diagnostic{code, message, {}}); }
bool validRelation(Relation relation) {
    return relation == Relation::Unknown || relation == Relation::Friendly ||
           relation == Relation::Neutral || relation == Relation::Hostile;
}
}

Result<void> validateSource(const SourceContext& source) {
    if (source.kind != SourceKind::Actor && source.kind != SourceKind::Environment)
        return fail(Error::Invalid, "invalid source kind");
    if ((source.instigator && !source.instigator->valid()) ||
        (source.directSource && !source.directSource->valid()))
        return fail(Error::Invalid, "invalid source identity");
    if (source.kind == SourceKind::Actor && !source.instigator)
        return fail(Error::Invalid, "actor source requires an instigator identity");
    if (source.kind == SourceKind::Environment && source.instigator)
        return fail(Error::Invalid, "environment source cannot have an actor instigator");
    return {};
}

Result<void> RelationTable::set(std::uint64_t observer, std::uint64_t target, Relation relation) {
    if (!observer || !target || !validRelation(relation))
        return fail(Error::Invalid, "invalid team identity or relation");
    auto entry = std::find_if(_entries.begin(), _entries.end(), [&](const Entry& e) {
        return e.observer == observer && e.target == target;
    });
    if (entry != _entries.end() && entry->relation == relation) return {};
    if (_version == std::numeric_limits<std::uint64_t>::max())
        return fail(Error::Limit, "relation version exhausted");
    if (entry == _entries.end()) _entries.push_back({observer, target, relation});
    else entry->relation = relation;
    ++_version;
    return {};
}

Relation RelationTable::query(std::uint64_t observer, std::uint64_t target) const noexcept {
    if (!observer || !target) return Relation::Unknown;
    for (const auto& entry : _entries)
        if (entry.observer == observer && entry.target == target) return entry.relation;
    if (_sameTeamFriendly && observer == target) return Relation::Friendly;
    return Relation::Unknown;
}

Result<TargetDecision> filterTarget(const SourceContext& source, const CombatantView& target,
                                    const RelationTable& relations, TargetFilter filter) {
    if (auto result = validateSource(source); !result) return std::unexpected(result.error());
    if (!target.ref.valid() || (filter.operation != Operation::Damage && filter.operation != Operation::Heal) ||
        (filter.allowed & ~std::uint32_t{63}) || (target.capabilities & ~std::uint32_t{3}))
        return fail(Error::Invalid, "invalid target, operation or filter mask");
    if ((source.instigator && source.instigator->domain != target.ref.domain) ||
        (source.directSource && source.directSource->domain != target.ref.domain))
        return fail(Error::Stale, "source and target belong to different simulation domains");
    if (!target.active) return TargetDecision{false, Denial::Inactive};
    const auto required = filter.operation == Operation::Damage ? Capability::Damageable : Capability::Healable;
    if (!(target.capabilities & mask(required))) return TargetDecision{false, Denial::MissingCapability};
    if (source.kind == SourceKind::Environment) {
        const bool allowed = (filter.allowed & mask(AllowedTarget::Environment)) != 0;
        return TargetDecision{allowed, allowed ? Denial::None : Denial::EnvironmentForbidden};
    }
    if (*source.instigator == target.ref) {
        const bool allowed = (filter.allowed & mask(AllowedTarget::Self)) != 0;
        return TargetDecision{allowed, allowed ? Denial::None : Denial::SelfForbidden, Relation::Friendly};
    }
    const auto relation = relations.query(source.team, target.team);
    auto category = AllowedTarget::Unknown;
    if (relation == Relation::Friendly) category = AllowedTarget::Friendly;
    if (relation == Relation::Neutral) category = AllowedTarget::Neutral;
    if (relation == Relation::Hostile) category = AllowedTarget::Hostile;
    const bool allowed = (filter.allowed & mask(category)) != 0;
    const auto denial = allowed ? Denial::None :
        (relation == Relation::Unknown ? Denial::UnknownRelation : Denial::RelationForbidden);
    return TargetDecision{allowed, denial, relation};
}

Result<std::vector<HitCandidate>> selectCandidates(std::span<const HitCandidate> candidates, CandidateScope scope) {
    if (scope != CandidateScope::Target && scope != CandidateScope::Part)
        return fail(Error::Invalid, "invalid candidate scope");
    for (const auto& candidate : candidates)
        if (!candidate.target.valid() || !candidate.collider)
            return fail(Error::Invalid, "candidate requires a mapped target and collider identity");
    std::vector<HitCandidate> sorted(candidates.begin(), candidates.end());
    std::sort(sorted.begin(), sorted.end(), [scope](const auto& a, const auto& b) {
        if (a.target != b.target) return a.target < b.target;
        if (scope == CandidateScope::Part && a.part != b.part) return a.part < b.part;
        if (a.priority != b.priority) return a.priority > b.priority;
        return std::tie(a.collider, a.part) < std::tie(b.collider, b.part);
    });
    sorted.erase(std::unique(sorted.begin(), sorted.end(), [scope](const auto& a, const auto& b) {
        return a.target == b.target && (scope == CandidateScope::Target || a.part == b.part);
    }), sorted.end());
    return sorted;
}

} // namespace ayt::combat
