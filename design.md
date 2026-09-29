# AYCombat v0.1 contracts

2026-09-29. Execution baseline: [engine implementation plan](../../AYDocs/AYCOMBAT-IMPLEMENTATION-PLAN.md).

## Delivered scope

Core is C++23 and standard-library-only. It owns no Entity, World, Stats pool or game object. It provides owned provenance values, directional team relations, pure operation permission checks, stable selection of already-mapped collider candidates, and a single-owner hit ledger. The optional Stats target executes a real immutable CalculationPlan and commits one persistent pool in one caller-owned StatSet.

Ordered rule predicates, block/zero/actual result classification and explicit life policy are delivered. AYCombat::Entity optionally binds real World/Entity/Stats components with owned outcome delivery. Physics mapping, automatic attack/skill scheduling, parry/redirection strategies, cross-object reactions, runtime save/load, large-scale benchmarks and Ability/Combo runtime remain planned. Public scope is partial; plan-stage acceptance is recorded separately.

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

CombatOutcome Applied confirms a hit: successful numerical settlement, including zero, or a block without numerical mutation. OutcomeKind distinguishes Rejected/Blocked/Zero/Damage/Heal. Rejected preserves the base target denial or matching rule ID with no reservation/pool change. Errors are distinct. Actual delta is signed using the Stats convention, not the requested/calculated amount. Outcome identity is `(attack.domain,attack.id,request)`.

preview checks supplied views/relations/rules and performs pure computation only for a passing request. It does not validate a live attack, resource availability or a future target version, and does not alter ledger/state/life. Base permission and the additional rule decision are separate fields.

## Rules, life and result delivery

RuleRegistry validates explicit nonzero unique IDs, orders Permission before Response, descending priority then ascending ID, and stops on the first matching predicate. Permission may Reject; Response may Reject or Block. Standard IDs 1/2/3 are damage invulnerability/type immunity/block, and never reject healing. Custom registries choose their own stable identities. Rules are immutable; predicates are pure and exceptions become pre-commit diagnostics. Friendly fire is the explicit TargetFilter mask. No numeric modifiers belong here; mitigation formulas remain Stats plans.

LifeTracker owns a resource policy, current Alive/Downed/Dead phase and transition sequence. It compares the resource before and after the entire atomic Stats settlement; an intermediate zero followed by restoration within that transaction is not death. Actual positive-to-zero depletes Alive; additional zero changes do not repeat death. Healing zero-to-positive revives only when policy enables it. Initial phase is supplied explicitly; Entity configureLife reads the selected persistent pool and handles initial zero as depleted. External Stats writes do not automatically invoke Combat life tracking; the owner must observe their successful settlement or reconfigure/rebind policy. Combat owns no duplicate status/effect clock.

SettlementTarget exposes const state inspection and the owner's controlled atomic commit. It must return Stats's one Change per mutation semantics; no const_cast or external pool mutation is used. EntityBinding combines StatsSystem World token, EntityHandle ID/version (version+1 in CombatantRef), Stats generation and Combat component generation. Resolve rejects removed/replaced/reconfigured components, stale handles, another World incarnation or replicas. Requests use current component team/capabilities/immunity/block facts, while source team/provenance stays the chosen retained snapshot. Retained source identities do not require a live source.

CombatSystem submits synchronously on a single owner; its Sim hook does not schedule attacks. Submission reentry/rule replacement is rejected. It reserves one notification change slot before settle and retains at most 1024 owned outcomes, including ordinary rejections. Overflow rejects before mutation; successful duplicate errors produce no new outcome. takeOutcomes transfers results without callbacks; consumers run afterward. Stats NextTick commits reserve their separate bounded notification queue before mutation and deliver on the next Stats Sim update. Notification exceptions are post-commit diagnostics and cannot release completed hit allowance. Reconfiguration/checkpoint restore discard old Stats notifications. Combat outcomes remain owned values after Entity deletion; the World-owned system/outbox is destroyed on World teardown.

Life/immuneTypes/attacks/outcomes are runtime state; serializable basic team/capability/invulnerability/block fields do not constitute a Combat checkpoint. Pausing/request production, externally written resources and consumers' own replay handling remain explicit owner responsibilities. General allocator failure and violation of pure-extension/owner contracts are outside the retryable transaction guarantee.

## Dependencies and deferred work

AYCombat::Core has no Stats/Entity/Physics dependency. AYCombat::Stats links Core and AYStats::Core. Optional AYCombat::Entity links Stats adapter and AYStats::Entity; it is assembled after those targets in the root build. Standalone Entity configuration fails clearly. Root opt-in requires the local checkouts; absent Stats must disable the adapter or fail configuration clearly. No remote/submodule URL is invented.

A single StatSet transaction is not a cross-object transaction. Multi-pool shield settlement, reflection/lifesteal and batch completion must use validated Stats contracts in later stages. Geometry, animation and Combo decisions consume Combat results in higher or optional layers; Core does not know skill graphs.

## Validation

AYCombat_UnitTests partitions Core/ledger as fast and real Stats integration as integration, with disjoint full-tier coverage. AYCombat_Example builds and runs a real 30-damage/70-health scenario. Core-only compilation disables the Stats adapter and excludes its integration suite. Use the engine inventory auditor and case reports for authoritative case counts; CTest partition count is not case count.
