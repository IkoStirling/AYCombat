#pragma once
#include <AYCombat/Combat.h>
#include <functional>

namespace ayt::combat {
using RuleId = std::uint64_t;
enum class RuleStage { Permission, Response };
enum class RuleAction { Pass, Reject, Block };
/// Current facts are supplied by the owner; Combat does not own buff timers.
struct RuleFacts {
    bool invulnerable = false, blocking = false;
    std::vector<std::uint64_t> immuneTypes;
};
struct RuleContext {
    const SourceContext& source;
    const CombatantView& target;
    Operation operation;
    std::uint64_t damageType;
    const RuleFacts& facts;
};
/// Predicate must be pure. Identity is explicit and stable across registration order.
struct Rule {
    RuleId id = 0;
    RuleStage stage = RuleStage::Permission;
    std::int32_t priority = 0;
    RuleAction action = RuleAction::Reject;
    std::function<bool(const RuleContext&)> matches;
};
struct RuleDecision { RuleAction action = RuleAction::Pass; RuleId reason = 0; };
/// Immutable ordering: stage, descending priority, ascending stable ID; first match wins.
class RuleRegistry {
public:
    static Result<RuleRegistry> compile(std::vector<Rule> rules);
    Result<RuleDecision> evaluate(const RuleContext& context) const;
    std::span<const Rule> rules() const noexcept { return _rules; }
private:
    std::vector<Rule> _rules;
};
/// IDs 1/2/3: invulnerability/type immunity/block. Friendly fire is TargetFilter policy.
Result<RuleRegistry> standardRules();
} // namespace ayt::combat
