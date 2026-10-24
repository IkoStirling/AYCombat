#include <AYTest.h>
#include <AYCombat/InitiativeClock.h>

using namespace ayt::combat;

TEST_SUITE(CombatInitiativeClockTests)

TEST_CASE(equal_rates_form_round_style_order_without_double_actions) {
    auto clock = InitiativeClock::create(10).value();
    CHECK_TRUE(clock.addActor(2, 10).has_value());
    CHECK_TRUE(clock.addActor(1, 10).has_value());
    CHECK_TRUE(clock.advance(1).has_value());
    CHECK_TRUE(clock.beginNext() == 1);
    CHECK_FALSE(clock.beginNext().has_value());
    CHECK_TRUE(clock.finish(1).has_value());
    CHECK_TRUE(clock.finish(1).error().code == Error::Stale);
    CHECK_TRUE(clock.beginNext() == 2);
    CHECK_TRUE(clock.finish(2).has_value());
    CHECK_FALSE(clock.beginNext().has_value());
    CHECK_TRUE(clock.advance(1).has_value());
    CHECK_TRUE(clock.beginNext() == 1);
}

TEST_CASE(different_rates_keep_earliest_ready_tick_across_large_steps) {
    auto clock = InitiativeClock::create(10).value();
    CHECK_TRUE(clock.addActor(1, 2).has_value());
    CHECK_TRUE(clock.addActor(2, 5).has_value());
    CHECK_TRUE(clock.advance(5).has_value());
    CHECK_TRUE(clock.actor(1)->readyTick == 5);
    CHECK_TRUE(clock.actor(2)->readyTick == 2);
    CHECK_TRUE(clock.beginNext() == 2);
    CHECK_TRUE(clock.advance(5).has_value());
    CHECK_TRUE(clock.actor(1)->readyTick == 5);
    CHECK_TRUE(clock.finish(2).has_value());
    CHECK_TRUE(clock.beginNext() == 1);
    CHECK_TRUE(clock.finish(1).has_value());
    CHECK_TRUE(clock.advance(2).has_value());
    CHECK_TRUE(clock.beginNext() == 2);
}

TEST_CASE(snapshot_restore_and_invalid_candidates_are_atomic) {
    auto clock = InitiativeClock::create(10).value();
    CHECK_TRUE(clock.addActor(1, 10).has_value());
    CHECK_TRUE(clock.addActor(2, 3).has_value());
    CHECK_TRUE(clock.advance(1).has_value());
    CHECK_TRUE(clock.beginNext() == 1);
    const auto saved = clock.snapshot();
    auto restored = InitiativeClock::create(10).value();
    CHECK_TRUE(restored.restore(saved).has_value());
    CHECK_TRUE(restored.snapshot().actors == saved.actors);
    auto invalid = saved;
    invalid.actors.push_back(invalid.actors.front());
    CHECK_FALSE(restored.restore(invalid).has_value());
    CHECK_TRUE(restored.snapshot().actors == saved.actors);
    invalid = saved;
    invalid.actors[1].phase = InitiativeClock::Phase::Acting;
    invalid.actors[1].gauge = 0;
    CHECK_FALSE(restored.restore(invalid).has_value());
    CHECK_TRUE(restored.finish(1).has_value());
    CHECK_TRUE(restored.removeActor(1).has_value());
    CHECK_TRUE(restored.advance(3).has_value());
    CHECK_TRUE(restored.beginNext() == 2);
}

TEST_CASE(overflow_and_invalid_registration_do_not_advance) {
    CHECK_FALSE(InitiativeClock::create(0).has_value());
    auto clock = InitiativeClock::create(1).value();
    CHECK_FALSE(clock.addActor(0, 1).has_value());
    CHECK_TRUE(clock.addActor(1, 1).has_value());
    CHECK_FALSE(clock.addActor(1, 1).has_value());
    auto saved = clock.snapshot();
    saved.tick = UINT64_MAX;
    CHECK_TRUE(clock.restore(saved).has_value());
    CHECK_TRUE(clock.advance(1).error().code == Error::Limit);
    CHECK_TRUE(clock.tick() == UINT64_MAX
        && clock.actor(1)->phase == InitiativeClock::Phase::Charging);
}

TEST_SUITE_END
