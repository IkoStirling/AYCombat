# AYCombat v0.1 contracts

2026-09-29. Execution baseline: [engine implementation plan](../../AYDocs/AYCOMBAT-IMPLEMENTATION-PLAN.md).

## Delivered scope

Core is C++23 and standard-library-only. It owns no Entity, World, Stats pool or game object. It provides owned provenance values, directional team relations, pure operation permission checks, stable selection of already-mapped collider candidates, and a single-owner hit ledger. The optional Stats target executes a real immutable CalculationPlan and commits one persistent pool in one caller-owned StatSet.

Entity/physics mapping, automatic Host ticking, life phases, result events, rules for block/parry, cross-object reactions, save/load, large-scale benchmarks and Ability/Combo runtime remain unimplemented. Public scope is partial; plan-stage acceptance is recorded separately.

## Identity and ownership

CombatantRef `(domain,object,generation)` is valid only with nonzero fields. Domain is a World/session generation chosen by the host. Core never turns it into a pointer. The host validates the current target binding before settle. SourceContext retains an Actor instigator identity or an Environment source without instigator; optional direct source, attribution, team snapshot, ability and effect identities remain owned values after source destruction. Source identities must belong to the target simulation domain.

Relations and views are snapshots supplied by the single simulation owner. Current actor lookup is not required for a deliberately retained source snapshot. Team zero means unknown. Explicit directional table entries override the opt-in same-team-friendly fallback. Unknown does not imply hostility. Self and Environment permission are separate from team relations. Invalid enum/mask/identity is an error; missing capability, inactive target and forbidden relations are ordinary denials.

## Candidate and hit semantics

Candidates must already have host-validated target/collider mappings. selectCandidates groups by target generation, optionally by part, selecting highest priority then lowest collider/part; target ordering is stable. Physics callback sequence is not authority.

An attack has a half-open tick window, one policy and an owner-assigned monotonic ID within its domain. The ledger cannot be copied; move only when no outstanding lease/adapter call borrows it. Target policy ignores part/segment; Part uses part; Segment uses segment; Periodic uses target and minimum ticks since every prior successful hit. Request ticks cannot go backwards within an active attack.

Each request identity is local to its attack. reserve checks structural input, active window, request replay, logical allowance and capacity before inserting a Pending record. Failed work releases to Retryable while retaining the logical key; retry cannot change that key. Every retry increments an attempt identity, so an old lease cannot release or complete the new attempt. complete changes the existing record to Completed without allocation. Success, including zero numerical change, consumes allowance.

Completed IDs are retained even after a periodic allowance opens again. Closing an attack releases its records, rejects pending work, and retains domain high-water marks to reject old or reopened attack IDs. High-water marks do not invalidate still-active lower attack IDs. Domain/active-attack/per-attack-request limits are explicit. Recreate the ledger when its host simulation domain lifetime ends; do not recycle request IDs to bypass retention.

## Numerical adapter and completion point

settle verifies target/hit binding, operation permission and calculation identity/tick, then reserves the hit. It validates a persistent pool and expected StatSet version, executes the Stats plan with caller inputs, and commits Increase/Decrease through Stats. The caller chooses the formula and sampled inputs; Combat has no implicit armor/shield formula or second pool.

Numerical failure releases allowance via a guard, including exceptions unwinding before commit. Stats error messages/stage paths remain owned diagnostics; stale state maps to Combat Stale. Negative output is invalid rather than healing. Capacity rejection occurs before numerical mutation.

After a successful Stats commit there are no callbacks or new allocations: the valid pending lease completes and the owned settlement is moved into CombatOutcome. This assumes the single-owner and pure-extension contracts; ledger mutation by an extension/concurrent thread is prohibited. Breaking the post-commit completion invariant terminates rather than returning a retryable error after applying damage.

CombatOutcome Applied means successful permission and numerical settlement, including zero damage; it is not a death or automatic damage event. Rejected preserves a denial with no reservation or pool change. Errors are distinct. Actual delta is signed using the Stats convention, not the requested/calculated amount. Outcome identity is `(attack.domain,attack.id,request)`.

preview only checks supplied views/relations and performs pure computation. It does not validate a live attack, resource availability or a future target version, and does not alter ledger/state. No automatic events or reentrant reaction queue exists in this adapter.

## Dependencies and deferred work

AYCombat::Core has no Stats/Entity/Physics dependency. AYCombat::Stats links Core and AYStats::Core. Root opt-in requires the local checkouts; absent Stats must disable the adapter or fail configuration clearly. No remote/submodule URL is invented.

A single StatSet transaction is not a cross-object transaction. Multi-pool shield settlement, reflection/lifesteal, host bindings and batch completion must use validated Stats contracts in later stages. Life phase, geometry, animation and Combo decisions consume Combat results in higher or optional layers; Core does not know skill graphs.

## Validation

AYCombat_UnitTests partitions Core/ledger as fast and real Stats integration as integration, with disjoint full-tier coverage. AYCombat_Example builds and runs a real 30-damage/70-health scenario. Core-only compilation disables the Stats adapter and excludes its integration suite. Use the engine inventory auditor and case reports for authoritative case counts; CTest partition count is not case count.
