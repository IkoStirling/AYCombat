#include <AYTest.h>
#include <AYCombat/Rules.h>
#include "Fixtures.h"

TEST_SUITE(CombatRuleTests)
TEST_CASE(stage_priority_and_stable_id_ignore_registration_order) {
    const auto yes = [](const RuleContext&) { return true; };
    auto rules = RuleRegistry::compile({{3, RuleStage::Response, 999, RuleAction::Block, yes},
        {7, RuleStage::Permission, 10, RuleAction::Reject, yes},
        {5, RuleStage::Permission, 10, RuleAction::Reject, yes}}).value();
    const auto s = source(); const auto t = target(); const RuleFacts facts;
    CHECK_TRUE(rules.evaluate({s, t, Operation::Damage, 0, facts})->reason == 5);
}
TEST_CASE(duplicate_invalid_and_throwing_rules_report_diagnostics) {
    const Rule rule{1, RuleStage::Permission, 0, RuleAction::Reject, [](const RuleContext&) { return true; }};
    CHECK_TRUE(RuleRegistry::compile({rule, rule}).error().code == Error::Duplicate);
    CHECK_FALSE(RuleRegistry::compile({{}}).has_value());
    auto throwing = rule; throwing.matches = [](const RuleContext&) -> bool { throw std::runtime_error("bad rule"); };
    const auto s = source(); const auto t = target(); const RuleFacts facts;
    CHECK_FALSE(RuleRegistry::compile({throwing})->evaluate({s,t,Operation::Damage,0,facts}).has_value());
}
TEST_CASE(standard_rules_distinguish_immunity_block_and_healing) {
    const auto rules = standardRules().value(); const auto s = source(); const auto t = target();
    RuleFacts facts{true, true, {9}};
    CHECK_TRUE(rules.evaluate({s,t,Operation::Damage,9,facts})->reason == 1);
    facts.invulnerable = false;
    CHECK_TRUE(rules.evaluate({s,t,Operation::Damage,9,facts})->reason == 2);
    CHECK_TRUE(rules.evaluate({s,t,Operation::Damage,8,facts})->action == RuleAction::Block);
    CHECK_TRUE(rules.evaluate({s,t,Operation::Heal,9,facts})->action == RuleAction::Pass);
}
TEST_SUITE_END
