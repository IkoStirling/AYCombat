#include <AYCombat/Rules.h>
#include <algorithm>
#include <exception>

namespace ayt::combat {
Result<RuleRegistry> RuleRegistry::compile(std::vector<Rule> rules) {
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const auto& r = rules[i];
        if (!r.id || !r.matches || (r.stage != RuleStage::Permission && r.stage != RuleStage::Response) ||
            (r.action != RuleAction::Reject && r.action != RuleAction::Block) ||
            (r.stage == RuleStage::Permission && r.action == RuleAction::Block))
            return std::unexpected(Diagnostic{Error::Invalid, "invalid rule identity/stage/action/predicate", {}});
        for (std::size_t j = 0; j < i; ++j) if (rules[j].id == r.id)
            return std::unexpected(Diagnostic{Error::Duplicate, "duplicate rule identity", {}});
    }
    std::sort(rules.begin(), rules.end(), [](const auto& a, const auto& b) {
        if (a.stage != b.stage) return a.stage < b.stage;
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.id < b.id;
    });
    RuleRegistry result; result._rules = std::move(rules); return result;
}
Result<RuleDecision> RuleRegistry::evaluate(const RuleContext& context) const {
    try {
        for (const auto& r : _rules) if (r.matches(context)) return RuleDecision{r.action, r.id};
    } catch (const std::exception& e) {
        return std::unexpected(Diagnostic{Error::Invalid, e.what(), "rule predicate"});
    } catch (...) {
        return std::unexpected(Diagnostic{Error::Invalid, "rule predicate threw", "rule predicate"});
    }
    return RuleDecision{};
}
Result<RuleRegistry> standardRules() {
    return RuleRegistry::compile({
        {1, RuleStage::Permission, 100, RuleAction::Reject, [](const RuleContext& c) {
            return c.operation == Operation::Damage && c.facts.invulnerable;
        }},
        {2, RuleStage::Permission, 90, RuleAction::Reject, [](const RuleContext& c) {
            return c.operation == Operation::Damage && c.damageType &&
                std::find(c.facts.immuneTypes.begin(), c.facts.immuneTypes.end(), c.damageType) != c.facts.immuneTypes.end();
        }},
        {3, RuleStage::Response, 0, RuleAction::Block, [](const RuleContext& c) {
            return c.operation == Operation::Damage && c.facts.blocking;
        }}
    });
}
} // namespace ayt::combat
