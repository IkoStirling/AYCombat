#include <AYCombat/Action2DWorldCommit.h>

#include <AYStats/AssetIO.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <set>

namespace ayt::combat {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxBytes = 4u * 1024u * 1024u;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
bool hasNegativeNumber(const Json& value) {
    if (value.type() == Json::value_t::number_integer)
        return value.get<std::int64_t>() < 0;
    if (value.is_array() || value.is_object())
        for (const auto& child : value)
            if (hasNegativeNumber(child)) return true;
    return false;
}
Json refJson(CombatantRef ref) {
    return Json::array({ref.domain, ref.object, ref.generation});
}
CombatantRef readRef(const Json& value) {
    if (!value.is_array() || value.size() != 3) throw std::runtime_error("invalid combatant ref");
    return {value.at(0).get<std::uint64_t>(), value.at(1).get<std::uint64_t>(),
            value.at(2).get<std::uint64_t>()};
}
Json actionJson(const ayt::ay2d::ActionState2D& value) {
    return Json::array({value.contentFingerprint, value.nextTick, value.cycle,
        value.hitWindow, value.activeClip, value.frame, value.elapsedInFrame,
        value.facingRight, value.pendingClip, value.pendingFacingRight,
        value.pendingExpiresBefore, value.hitTargets});
}
ayt::ay2d::ActionState2D readAction(const Json& value) {
    if (!value.is_array() || value.size() != 12) throw std::runtime_error("invalid action state");
    ayt::ay2d::ActionState2D result;
    result.contentFingerprint = value.at(0).get<std::uint64_t>();
    result.nextTick = value.at(1).get<std::uint64_t>();
    result.cycle = value.at(2).get<std::uint64_t>();
    result.hitWindow = value.at(3).get<std::uint32_t>();
    result.activeClip = value.at(4).get<std::uint32_t>();
    result.frame = value.at(5).get<std::uint32_t>();
    result.elapsedInFrame = value.at(6).get<std::uint32_t>();
    result.facingRight = value.at(7).get<bool>();
    result.pendingClip = value.at(8).get<std::uint32_t>();
    result.pendingFacingRight = value.at(9).get<bool>();
    result.pendingExpiresBefore = value.at(10).get<std::uint64_t>();
    result.hitTargets = value.at(11).get<std::vector<std::uint64_t>>();
    if (!std::is_sorted(result.hitTargets.begin(), result.hitTargets.end())
        || std::adjacent_find(result.hitTargets.begin(), result.hitTargets.end())
            != result.hitTargets.end()) throw std::runtime_error("invalid action hit targets");
    return result;
}
Json hitJson(const HitLedgerState& value) {
    Json domains = Json::array(), attacks = Json::array();
    for (const auto& [id, highWater] : value.domainHighWater)
        domains.push_back(Json::array({id, highWater}));
    for (const auto& attack : value.attacks) {
        Json records = Json::array();
        for (const auto& record : attack.records)
            records.push_back(Json::array({record.request, record.tick,
                record.attempt, refJson(record.target), record.part,
                record.segment, record.completed}));
        attacks.push_back(Json::array({attack.ref.domain, attack.ref.id,
            static_cast<int>(attack.policy.scope), attack.policy.interval,
            attack.start, attack.end, attack.lastTick, std::move(records)}));
    }
    return Json{{"limits", Json::array({value.limits.domains,
        value.limits.attacks, value.limits.requestsPerAttack})},
        {"domains", std::move(domains)}, {"attacks", std::move(attacks)}};
}
HitLedgerState readHits(const Json& value) {
    HitLedgerState result;
    const auto& limits = value.at("limits");
    if (!limits.is_array() || limits.size() != 3)
        throw std::runtime_error("invalid hit limits");
    result.limits = {limits.at(0).get<std::size_t>(),
        limits.at(1).get<std::size_t>(), limits.at(2).get<std::size_t>()};
    if (!value.at("domains").is_array() || value.at("domains").size() > 65536
        || !value.at("attacks").is_array()
        || value.at("attacks").size() > 65536)
        throw std::runtime_error("invalid hit ledger size");
    for (const auto& domain : value.at("domains")) {
        if (!domain.is_array() || domain.size() != 2)
            throw std::runtime_error("invalid hit domain");
        result.domainHighWater.emplace_back(domain.at(0).get<std::uint64_t>(),
            domain.at(1).get<std::uint64_t>());
    }
    for (const auto& attack : value.at("attacks")) {
        if (!attack.is_array() || attack.size() != 8)
            throw std::runtime_error("invalid hit attack");
        HitLedgerAttackState saved;
        saved.ref = {attack.at(0).get<std::uint64_t>(),
            attack.at(1).get<std::uint64_t>()};
        saved.policy = {static_cast<HitScope>(attack.at(2).get<int>()),
            attack.at(3).get<std::uint64_t>()};
        saved.start = attack.at(4).get<std::uint64_t>();
        saved.end = attack.at(5).get<std::uint64_t>();
        saved.lastTick = attack.at(6).get<std::uint64_t>();
        if (!attack.at(7).is_array() || attack.at(7).size() > 65536)
            throw std::runtime_error("invalid hit record count");
        for (const auto& record : attack.at(7)) {
            if (!record.is_array() || record.size() != 7)
                throw std::runtime_error("invalid hit record");
            saved.records.push_back({record.at(0).get<std::uint64_t>(),
                record.at(1).get<std::uint64_t>(),
                record.at(2).get<std::uint64_t>(), readRef(record.at(3)),
                record.at(4).get<std::uint32_t>(),
                record.at(5).get<std::uint32_t>(), record.at(6).get<bool>()});
        }
        result.attacks.push_back(std::move(saved));
    }
    HitLedger check;
    if (!check.restore(result)) throw std::runtime_error("invalid hit ledger state");
    return result;
}
} // namespace

bool encodeAction2DWorldCandidate(const Action2DWorldCandidate& candidate,
                                  std::string& json, std::string* error) {
    if (candidate.actorIds.empty() || candidate.actorIds.size() > 64
        || candidate.actorIds.size() != candidate.actions.size()
        || !std::is_sorted(candidate.actorIds.begin(), candidate.actorIds.end())
        || std::adjacent_find(candidate.actorIds.begin(), candidate.actorIds.end())
            != candidate.actorIds.end()
        || candidate.targetIds.size() > 64
        || candidate.targetIds.size() != candidate.stats.size()
        || candidate.targetIds.size() != candidate.life.size())
        return fail(error, "invalid action candidate shape");
    try {
        std::set<CombatantRef> targetIds;
        for (const auto& ref : candidate.targetIds)
            if (!ref.valid() || !targetIds.insert(ref).second)
                return fail(error, "invalid action candidate targets");
        HitLedger checked;
        if (!checked.restore(candidate.hits))
            return fail(error, "invalid action candidate hits");
        Json root{{"version", 1}, {"actorIds", candidate.actorIds},
            {"actions", Json::array()}, {"targets", Json::array()},
            {"stats", Json::array()}, {"life", Json::array()},
            {"hits", hitJson(candidate.hits)}};
        for (const auto& state : candidate.actions)
            root["actions"].push_back(actionJson(state));
        for (std::size_t i = 0; i < candidate.stats.size(); ++i) {
            root["targets"].push_back(refJson(candidate.targetIds[i]));
            auto encoded = ayt::stats::encodeState(candidate.stats[i]);
            if (!encoded) return fail(error, "cannot encode candidate Stats");
            root["stats"].push_back(*encoded);
            if (candidate.life[i]) {
                const auto state = candidate.life[i]->snapshot();
                root["life"].push_back(Json::array({state.policy.resource,
                    static_cast<int>(state.policy.depleted),
                    state.policy.healingRevives, static_cast<int>(state.phase),
                    state.sequence}));
            } else root["life"].push_back(nullptr);
        }
        std::string worldJson, outboxJson;
        if (!candidate.world.encode(worldJson, error)
            || !candidate.outbox.encode(outboxJson, error)) return false;
        root["world"] = std::move(worldJson);
        root["outbox"] = std::move(outboxJson);
        auto encoded = root.dump();
        if (encoded.size() > kMaxBytes)
            return fail(error, "action candidate exceeds 4 MiB");
        json = std::move(encoded);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, "cannot encode action candidate");
    }
}

bool decodeAction2DWorldSnapshot(
    std::string_view json,
    std::span<const std::shared_ptr<const ayt::stats::Schema>> schemas,
    Action2DWorldSnapshot& out, std::string* error) {
    if (json.empty() || json.size() > kMaxBytes)
        return fail(error, "invalid action snapshot size");
    try {
        const auto root = Json::parse(json.begin(), json.end());
        if (!root.is_object() || root.size() != 9 || root.at("version") != 1
            || hasNegativeNumber(root))
            return fail(error, "invalid action snapshot version or fields");
        Action2DWorldSnapshot candidate;
        candidate.actorIds = root.at("actorIds").get<std::vector<std::uint64_t>>();
        const auto& actions = root.at("actions");
        const auto& targets = root.at("targets");
        const auto& stats = root.at("stats");
        const auto& life = root.at("life");
        if (candidate.actorIds.empty() || candidate.actorIds.size() > 64
            || !actions.is_array() || actions.size() != candidate.actorIds.size()
            || !targets.is_array() || targets.size() > 64
            || !stats.is_array() || stats.size() != targets.size()
            || !life.is_array() || life.size() != targets.size()
            || schemas.size() != targets.size()
            || !std::is_sorted(candidate.actorIds.begin(), candidate.actorIds.end())
            || std::adjacent_find(candidate.actorIds.begin(), candidate.actorIds.end())
                != candidate.actorIds.end())
            return fail(error, "invalid action snapshot shape");
        for (const auto& state : actions)
            candidate.actions.push_back(readAction(state));
        std::set<CombatantRef> seenTargets;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto ref = readRef(targets.at(i));
            if (!ref.valid() || !schemas[i] || !seenTargets.insert(ref).second)
                return fail(error, "invalid action snapshot target");
            candidate.targetIds.push_back(ref);
            auto decoded = ayt::stats::decodeState(
                stats.at(i).get<std::string>(), schemas[i]);
            if (!decoded) return fail(error, "invalid action snapshot Stats");
            candidate.stats.push_back(std::move(*decoded));
            if (life.at(i).is_null()) candidate.life.emplace_back(std::nullopt);
            else {
                const auto& saved = life.at(i);
                if (!saved.is_array() || saved.size() != 5)
                    return fail(error, "invalid action snapshot life");
                LifeTrackerState value{{saved.at(0).get<ayt::stats::StatId>(),
                    static_cast<LifePhase>(saved.at(1).get<int>()),
                    saved.at(2).get<bool>()},
                    static_cast<LifePhase>(saved.at(3).get<int>()),
                    saved.at(4).get<std::uint64_t>()};
                LifeTracker check(value.policy);
                if (!check.restore(value))
                    return fail(error, "invalid action snapshot life");
                candidate.life.emplace_back(value);
            }
        }
        candidate.hits = readHits(root.at("hits"));
        if (!candidate.world.decode(root.at("world").get<std::string>(), error)
            || !candidate.outbox.decode(root.at("outbox").get<std::string>(), error))
            return false;
        out = std::move(candidate);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, "invalid action snapshot JSON");
    }
}

} // namespace ayt::combat
