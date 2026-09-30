#include <AYTest.h>
#include "Fixtures.h"
#include <algorithm>
#include <array>

TEST_SUITE(CombatCoreTests)

TEST_CASE(directional_relationships_and_explicit_same_team_policy) {
    auto table = relations();
    CHECK_TRUE(table.query(10, 20) == Relation::Hostile);
    CHECK_TRUE(table.query(20, 10) == Relation::Unknown);
    CHECK_TRUE(table.query(10, 10) == Relation::Unknown);
    RelationTable teams(true);
    CHECK_TRUE(teams.query(10, 10) == Relation::Friendly);
    CHECK_TRUE(teams.query(10, 20) == Relation::Unknown);
    CHECK_TRUE(teams.query(0, 0) == Relation::Unknown);
    CHECK_TRUE(teams.set(10, 10, Relation::Hostile).has_value());
    CHECK_TRUE(teams.query(10, 10) == Relation::Hostile);
}

TEST_CASE(relation_changes_update_versions_without_noop_invalidation) {
    auto table = relations();
    const auto version = table.version();
    CHECK_TRUE(table.set(10, 20, Relation::Hostile).has_value());
    CHECK_TRUE(table.version() == version);
    CHECK_TRUE(table.set(10, 20, Relation::Friendly).has_value());
    CHECK_TRUE(table.version() == version + 1);
    CHECK_TRUE(table.set(0, 20, Relation::Hostile).error().code == Error::Invalid);
    CHECK_TRUE(table.set(10, 20, static_cast<Relation>(99)).error().code == Error::Invalid);
}

TEST_CASE(friendly_fire_and_healing_use_operation_specific_permission) {
    auto table = relations();
    CHECK_TRUE(filterTarget(source(), target(), table, {}).value().allowed);
    CHECK_TRUE(table.set(10, 20, Relation::Friendly).has_value());
    CHECK_TRUE(filterTarget(source(), target(), table, {}).value().denial == Denial::RelationForbidden);
    CHECK_TRUE(filterTarget(source(), target(), table, {Operation::Damage, mask(AllowedTarget::Friendly)}).value().allowed);
    CHECK_TRUE(filterTarget(source(), target(), table, {Operation::Heal, mask(AllowedTarget::Friendly)}).value().allowed);
    auto immune = target();
    immune.capabilities = mask(Capability::Healable);
    CHECK_TRUE(filterTarget(source(), immune, table, {Operation::Damage, mask(AllowedTarget::Friendly)}).value().denial == Denial::MissingCapability);
}

TEST_CASE(self_and_environment_are_explicit_categories) {
    auto table = relations();
    auto self = target(); self.ref = actor();
    CHECK_TRUE(filterTarget(source(), self, table, {}).value().denial == Denial::SelfForbidden);
    CHECK_TRUE(filterTarget(source(), self, table, {Operation::Damage, mask(AllowedTarget::Self)}).value().allowed);
    SourceContext environment{.kind = SourceKind::Environment};
    CHECK_TRUE(filterTarget(environment, target(), table, {}).value().denial == Denial::EnvironmentForbidden);
    CHECK_TRUE(filterTarget(environment, target(), table, {Operation::Damage, mask(AllowedTarget::Environment)}).value().allowed);
}

TEST_CASE(unknown_and_neutral_relations_do_not_implicitly_allow_damage) {
    RelationTable table;
    CHECK_TRUE(filterTarget(source(), target(), table, {}).value().denial == Denial::UnknownRelation);
    CHECK_TRUE(filterTarget(source(), target(), table, {Operation::Damage, mask(AllowedTarget::Unknown)}).value().allowed);
    CHECK_TRUE(table.set(10, 20, Relation::Neutral).has_value());
    CHECK_TRUE(filterTarget(source(), target(), table, {}).value().denial == Denial::RelationForbidden);
    CHECK_TRUE(filterTarget(source(), target(), table, {Operation::Damage, mask(AllowedTarget::Neutral)}).value().allowed);
}

TEST_CASE(source_snapshot_keeps_attribution_without_live_entity_resolution) {
    auto saved = source();
    saved.effect = 900; saved.ability = 7; saved.abilityInstance = 81;
    CHECK_TRUE(validateSource(saved).has_value());
    CHECK_TRUE(filterTarget(saved, target(), relations(), {}).value().allowed);
    CHECK_TRUE(saved.attribution == 100 && saved.directSource == actor(50) && saved.abilityInstance == 81);
    auto malformed = saved; malformed.instigator.reset();
    CHECK_TRUE(validateSource(malformed).error().code == Error::Invalid);
}

TEST_CASE(inactive_invalid_and_cross_domain_targets_are_distinct) {
    auto view = target(); view.active = false;
    CHECK_TRUE(filterTarget(source(), view, relations(), {}).value().denial == Denial::Inactive);
    view = target(); view.ref.generation = 0;
    CHECK_TRUE(filterTarget(source(), view, relations(), {}).error().code == Error::Invalid);
    view = target(); view.ref.domain = 2;
    CHECK_TRUE(filterTarget(source(), view, relations(), {}).error().code == Error::Stale);
    CHECK_TRUE(filterTarget(source(), target(), relations(), {Operation::Damage, 128}).error().code == Error::Invalid);
}

TEST_CASE(multiple_colliders_select_one_stable_target_and_highest_priority_part) {
    std::array<HitCandidate, 4> candidates{{{actor(2), 30, 1, 0}, {actor(2), 20, 2, 5},
                                         {actor(2), 10, 3, 5}, {actor(3), 1, 0, 0}}};
    const auto selected = selectCandidates(candidates, CandidateScope::Target).value();
    CHECK_TRUE(selected.size() == 2);
    CHECK_TRUE(selected[0].collider == 10 && selected[0].part == 3);
    std::reverse(candidates.begin(), candidates.end());
    const auto reordered = selectCandidates(candidates, CandidateScope::Target).value();
    CHECK_TRUE(reordered[0].collider == selected[0].collider && reordered[1].target == selected[1].target);
}

TEST_CASE(part_selection_and_target_generation_are_separate_identities) {
    const std::array<HitCandidate, 4> candidates{{{actor(2), 30, 1, 0}, {actor(2), 20, 1, 5},
                                                {actor(2), 10, 2, 5}, {actor(2, 2), 1, 1, 0}}};
    const auto selected = selectCandidates(candidates, CandidateScope::Part).value();
    CHECK_TRUE(selected.size() == 3);
    CHECK_TRUE(selected[0].collider == 20);
    const std::array<HitCandidate, 1> invalid{{{actor(2), 0, 1, 0}}};
    CHECK_TRUE(selectCandidates(invalid, CandidateScope::Target).error().code == Error::Invalid);
}

TEST_SUITE_END
