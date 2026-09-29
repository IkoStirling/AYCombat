#include <AYTest.h>
#include "Fixtures.h"

TEST_SUITE(CombatHitLedgerTests)

TEST_CASE(pending_hits_block_duplicate_requests_and_other_colliders) {
    auto book = ledger();
    const auto lease = book.reserve(hit()).value();
    CHECK_TRUE(book.reserve(hit()).error().code == Error::Pending);
    auto another = hit(2); another.part = 4;
    CHECK_TRUE(book.reserve(another).error().code == Error::Pending);
    CHECK_TRUE(book.close({1, 1}).error().code == Error::Pending);
    CHECK_TRUE(book.complete(lease));
    CHECK_TRUE(book.reserve(hit()).error().code == Error::Duplicate);
    CHECK_TRUE(book.reserve(another).error().code == Error::AlreadyHit);
}

TEST_CASE(release_allows_retry_without_rebinding_and_invalidates_old_lease) {
    auto book = ledger();
    const auto old = book.reserve(hit()).value();
    CHECK_TRUE(book.release(old));
    auto changed = hit(); changed.target = actor(3);
    CHECK_TRUE(book.reserve(changed).error().code == Error::Invalid);
    const auto retry = book.reserve(hit()).value();
    CHECK_FALSE(book.complete(old));
    CHECK_FALSE(book.release(old));
    CHECK_TRUE(book.complete(retry));
    CHECK_TRUE(book.retainedRequests() == 1);
}

TEST_CASE(periodic_allowance_does_not_erase_successful_request_identity) {
    auto book = ledger({HitScope::Periodic, 5});
    CHECK_TRUE(book.complete(book.reserve(hit(1, 0)).value()));
    CHECK_TRUE(book.reserve(hit(2, 4)).error().code == Error::AlreadyHit);
    CHECK_TRUE(book.complete(book.reserve(hit(2, 5)).value()));
    CHECK_TRUE(book.reserve(hit(1, 10)).error().code == Error::Duplicate);
    CHECK_TRUE(book.complete(book.reserve(hit(3, 10)).value()));
}

TEST_CASE(part_and_segment_policies_allow_only_explicit_independent_hits) {
    auto parts = ledger({HitScope::Part});
    CHECK_TRUE(parts.complete(parts.reserve(hit()).value()));
    auto second = hit(2); second.part = 1;
    CHECK_TRUE(parts.complete(parts.reserve(second).value()));
    auto segments = ledger({HitScope::Segment});
    CHECK_TRUE(segments.complete(segments.reserve(hit()).value()));
    second = hit(2); second.segment = 1;
    CHECK_TRUE(segments.complete(segments.reserve(second).value()));
    second.request = 3; second.part = 99;
    CHECK_TRUE(segments.reserve(second).error().code == Error::AlreadyHit);
}

TEST_CASE(reused_target_id_with_new_generation_has_new_hit_identity) {
    auto book = ledger();
    CHECK_TRUE(book.complete(book.reserve(hit()).value()));
    auto next = hit(2); next.target = actor(2, 2);
    CHECK_TRUE(book.complete(book.reserve(next).value()));
}

TEST_CASE(close_cleans_records_but_never_reopens_old_attack_identity) {
    auto book = ledger();
    CHECK_TRUE(book.complete(book.reserve(hit()).value()));
    CHECK_TRUE(book.close({1, 1}).has_value());
    CHECK_TRUE(book.activeAttacks() == 0 && book.retainedRequests() == 0);
    CHECK_TRUE(book.reserve(hit()).error().code == Error::Closed);
    CHECK_TRUE(book.open({1, 1}, {}, 0, 100).error().code == Error::Stale);
    CHECK_TRUE(book.open({1, 2}, {}, 0, 100).has_value());
}

TEST_CASE(half_open_windows_and_monotonic_tick_order_reject_late_work) {
    HitLedger book;
    CHECK_TRUE(book.open({1, 1}, {}, 5, 10).has_value());
    CHECK_TRUE(book.reserve(hit(1, 4)).error().code == Error::Stale);
    CHECK_TRUE(book.reserve(hit(1, 10)).error().code == Error::Stale);
    auto pending = book.reserve(hit(1, 7)).value();
    CHECK_TRUE(book.release(pending));
    CHECK_TRUE(book.reserve(hit(1, 6)).error().code == Error::Stale);
    CHECK_TRUE(book.complete(book.reserve(hit(1, 9)).value()));
}

TEST_CASE(capacity_exhaustion_is_explicit_and_never_drops_successful_records) {
    HitLedger book({1, 1, 1});
    CHECK_TRUE(book.open({1, 1}, {HitScope::Segment}, 0, 100).has_value());
    CHECK_TRUE(book.open({1, 2}, {}, 0, 100).error().code == Error::Limit);
    CHECK_TRUE(book.complete(book.reserve(hit()).value()));
    auto next = hit(2); next.segment = 1;
    CHECK_TRUE(book.reserve(next).error().code == Error::Limit);
    CHECK_TRUE(book.reserve(hit()).error().code == Error::Duplicate);
    CHECK_TRUE(book.close({1, 1}).has_value());
    CHECK_TRUE(book.open({2, 1}, {}, 0, 100).error().code == Error::Limit);
    CHECK_TRUE(book.open({1, 2}, {}, 0, 100).has_value());
}

TEST_CASE(invalid_policies_and_cross_domain_work_do_not_reserve_capacity) {
    HitLedger book;
    CHECK_TRUE(book.open({1, 1}, {HitScope::Periodic, 0}, 0, 100).error().code == Error::Invalid);
    CHECK_TRUE(book.open({1, 1}, {HitScope::Target, 5}, 0, 100).error().code == Error::Invalid);
    CHECK_TRUE(book.open({1, 1}, {}, 10, 10).error().code == Error::Invalid);
    CHECK_TRUE(book.open({1, 1}, {}, 0, 100).has_value());
    auto foreign = hit(); foreign.target.domain = 2;
    CHECK_TRUE(book.reserve(foreign).error().code == Error::Stale);
    CHECK_TRUE(book.retainedRequests() == 0);
}

TEST_CASE(new_attack_does_not_invalidate_still_active_older_attack) {
    auto book = ledger();
    CHECK_TRUE(book.open({1, 2}, {}, 0, 100).has_value());
    CHECK_TRUE(book.complete(book.reserve(hit()).value()));
    auto next = hit(); next.attack.id = 2;
    CHECK_TRUE(book.complete(book.reserve(next).value()));
}

TEST_SUITE_END
