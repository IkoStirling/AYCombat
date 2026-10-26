#include <AYCombat/Action3DWorldCommit.h>

#include <AYStats/AssetIO.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>

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
    if (!value.is_array() || value.size() != 3)
        throw std::runtime_error("invalid combatant ref");
    return {value.at(0).get<std::uint64_t>(),
        value.at(1).get<std::uint64_t>(), value.at(2).get<std::uint64_t>()};
}
Json actionJson(const ayt::ability::FixedActionSnapshot& value) {
    return Json::array({value.version, value.definitionFingerprint,
        value.lastTick, value.action, value.bufferedAction, value.execution,
        value.startTick, value.bufferedTick, static_cast<int>(value.lastPhase)});
}
ayt::ability::FixedActionSnapshot readAction(const Json& value) {
    if (!value.is_array() || value.size() != 9)
        throw std::runtime_error("invalid 3D action state");
    ayt::ability::FixedActionSnapshot result;
    result.version = value.at(0).get<std::uint32_t>();
    result.definitionFingerprint = value.at(1).get<std::uint64_t>();
    result.lastTick = value.at(2).get<std::uint64_t>();
    result.action = value.at(3).get<ayt::ability::Id>();
    result.bufferedAction = value.at(4).get<ayt::ability::Id>();
    result.execution = value.at(5).get<std::uint64_t>();
    result.startTick = value.at(6).get<std::uint64_t>();
    result.bufferedTick = value.at(7).get<std::uint64_t>();
    result.lastPhase = static_cast<ayt::ability::FixedActionPhase>(
        value.at(8).get<int>());
    if (result.version != 1
        || result.lastPhase > ayt::ability::FixedActionPhase::Recovery)
        throw std::runtime_error("unsupported 3D action state");
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
    if (!limits.is_array() || limits.size() != 3
        || !value.at("domains").is_array()
        || value.at("domains").size() > 65536
        || !value.at("attacks").is_array()
        || value.at("attacks").size() > 65536)
        throw std::runtime_error("invalid hit ledger shape");
    result.limits = {limits.at(0).get<std::size_t>(),
        limits.at(1).get<std::size_t>(), limits.at(2).get<std::size_t>()};
    for (const auto& domain : value.at("domains")) {
        if (!domain.is_array() || domain.size() != 2)
            throw std::runtime_error("invalid hit domain");
        result.domainHighWater.emplace_back(domain.at(0).get<std::uint64_t>(),
            domain.at(1).get<std::uint64_t>());
    }
    for (const auto& attack : value.at("attacks")) {
        if (!attack.is_array() || attack.size() != 8
            || !attack.at(7).is_array() || attack.at(7).size() > 65536)
            throw std::runtime_error("invalid hit attack");
        HitLedgerAttackState saved;
        saved.ref = {attack.at(0).get<std::uint64_t>(),
            attack.at(1).get<std::uint64_t>()};
        saved.policy = {static_cast<HitScope>(attack.at(2).get<int>()),
            attack.at(3).get<std::uint64_t>()};
        saved.start = attack.at(4).get<std::uint64_t>();
        saved.end = attack.at(5).get<std::uint64_t>();
        saved.lastTick = attack.at(6).get<std::uint64_t>();
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

bool encodeAction3DWorldCandidate(const Action3DWorldCandidate& candidate,
                                  std::string& json, std::string* error) {
    if (candidate.targetIds.size() > 64
        || candidate.targetIds.size() != candidate.stats.size()
        || candidate.targetIds.size() != candidate.life.size()
        || candidate.action.version != 1
        || candidate.action.lastPhase > ayt::ability::FixedActionPhase::Recovery)
        return fail(error, "invalid 3D action candidate shape");
    try {
        std::set<CombatantRef> seen;
        for (const auto& ref : candidate.targetIds)
            if (!ref.valid() || !seen.insert(ref).second)
                return fail(error, "invalid 3D target identity");
        HitLedger checked;
        if (!checked.restore(candidate.hits))
            return fail(error, "invalid 3D candidate hits");
        Json root{{"version", 1}, {"action", actionJson(candidate.action)},
            {"targets", Json::array()}, {"stats", Json::array()},
            {"life", Json::array()}, {"hits", hitJson(candidate.hits)}};
        for (std::size_t i = 0; i < candidate.stats.size(); ++i) {
            root["targets"].push_back(refJson(candidate.targetIds[i]));
            auto encoded = ayt::stats::encodeState(candidate.stats[i]);
            if (!encoded) return fail(error, "cannot encode candidate Stats");
            root["stats"].push_back(*encoded);
            if (candidate.life[i]) {
                const auto value = candidate.life[i]->snapshot();
                root["life"].push_back(Json::array({value.policy.resource,
                    static_cast<int>(value.policy.depleted),
                    value.policy.healingRevives, static_cast<int>(value.phase),
                    value.sequence}));
            } else root["life"].push_back(nullptr);
        }
        std::string worldJson, outboxJson;
        if (!candidate.world.encode(worldJson, error)
            || !candidate.outbox.encode(outboxJson, error)) return false;
        root["world"] = std::move(worldJson);
        root["outbox"] = std::move(outboxJson);
        auto encoded = root.dump();
        if (encoded.size() > kMaxBytes)
            return fail(error, "3D action candidate exceeds 4 MiB");
        json = std::move(encoded);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, "cannot encode 3D action candidate");
    }
}

bool decodeAction3DWorldSnapshot(
    std::string_view json,
    std::span<const std::shared_ptr<const ayt::stats::Schema>> schemas,
    Action3DWorldSnapshot& out, std::string* error) {
    if (json.empty() || json.size() > kMaxBytes)
        return fail(error, "invalid 3D action snapshot size");
    try {
        const auto root = Json::parse(json.begin(), json.end());
        if (!root.is_object() || root.size() != 8 || root.at("version") != 1
            || hasNegativeNumber(root))
            return fail(error, "invalid 3D action snapshot version or fields");
        Action3DWorldSnapshot candidate;
        candidate.action = readAction(root.at("action"));
        const auto& targets = root.at("targets");
        const auto& stats = root.at("stats");
        const auto& life = root.at("life");
        if (!targets.is_array() || targets.size() > 64
            || !stats.is_array() || stats.size() != targets.size()
            || !life.is_array() || life.size() != targets.size()
            || schemas.size() != targets.size())
            return fail(error, "invalid 3D action snapshot shape");
        std::set<CombatantRef> seen;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto ref = readRef(targets.at(i));
            if (!ref.valid() || !schemas[i] || !seen.insert(ref).second)
                return fail(error, "invalid 3D action target");
            candidate.targetIds.push_back(ref);
            auto decoded = ayt::stats::decodeState(
                stats.at(i).get<std::string>(), schemas[i]);
            if (!decoded) return fail(error, "invalid 3D action Stats");
            candidate.stats.push_back(std::move(*decoded));
            if (life.at(i).is_null()) candidate.life.emplace_back(std::nullopt);
            else {
                const auto& saved = life.at(i);
                if (!saved.is_array() || saved.size() != 5)
                    return fail(error, "invalid 3D action life");
                LifeTrackerState value{{saved.at(0).get<ayt::stats::StatId>(),
                    static_cast<LifePhase>(saved.at(1).get<int>()),
                    saved.at(2).get<bool>()},
                    static_cast<LifePhase>(saved.at(3).get<int>()),
                    saved.at(4).get<std::uint64_t>()};
                LifeTracker check(value.policy);
                if (!check.restore(value))
                    return fail(error, "invalid 3D action life");
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
        return fail(error, "invalid 3D action snapshot JSON");
    }
}

} // namespace ayt::combat
