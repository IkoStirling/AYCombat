# AYCombat v0.1 contracts

2026-09-30. Execution baseline and delivery evidence: [engine implementation plan](../../AYDocs/AYCOMBAT-IMPLEMENTATION-PLAN.md), sections 16–17.

## Delivered scope

Core is C++23 and standard-library-only. It owns no Entity, World, Stats pool or game object. It provides owned provenance values, directional team relations, pure operation permission checks, stable selection of already-mapped collider candidates, and a single-owner hit ledger. The optional Stats target executes a real immutable CalculationPlan and commits one persistent pool in one caller-owned StatSet.

Ordered rules, block/zero/actual result classification and explicit life policy are delivered. AYCombat::Entity adds a World-owned gameplay facade, default/explicit Stats recipes, generic Stats attribute changes and automatic internal life propagation. Optional Scene/Host targets install the complete dependencies for new Worlds. Generic permitted effects/area membership, physics, shields, attack/skill scheduling, parry/redirection, cross-object reactions, runtime save/load, scale benchmarks and Ability/Combo remain planned. Public scope is partial.

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

LifeTracker owns a resource policy, Alive/Downed/Dead phase and transition sequence. It observes Current fields or final authoritative current snapshots; Base/Capacity changes are not life deltas. Intermediate zero followed by restoration within one atomic transaction is not death. Positive-to-zero depletes Alive once; healing zero-to-positive revives only when enabled. Entity internal publication automatically covers direct commits, effects, capacity clamps and batch publication. Configure/restore initialize the current phase without manufacturing a hit; component generations still invalidate queued bindings. A raw caller-owned StatSet integration can still explicitly observe its own successful settlement. Combat owns no duplicate numerical clock.

SettlementTarget exposes const state inspection and the owner's controlled atomic commit. It must return Stats's one Change per mutation semantics; no const_cast or external pool mutation is used. EntityBinding combines StatsSystem World token, EntityHandle ID/version (version+1 in CombatantRef), Stats generation and Combat component generation. Resolve rejects removed/replaced/reconfigured components, stale handles, another World incarnation or replicas. Requests use current component team/capabilities/immunity/block facts, while source team/provenance stays the chosen retained snapshot. Retained source identities do not require a live source.

CombatSystem submits synchronously on a single owner; its Sim hook does not schedule attacks. Submission reentry/rule replacement is rejected. It reserves one notification change slot before settle and retains at most 1024 owned outcomes, including ordinary rejections. Overflow rejects before mutation; successful duplicate errors produce no new outcome. takeOutcomes transfers results without callbacks; consumers run afterward. Stats NextTick commits reserve their separate bounded notification queue before mutation and deliver on the next Stats Sim update. Notification exceptions are post-commit diagnostics and cannot release completed hit allowance. Reconfiguration/checkpoint restore discard old Stats notifications. Combat outcomes remain owned values after Entity deletion; the World-owned system/outbox is destroyed on World teardown.

Life/immuneTypes/attacks/outcomes are runtime state; serializable team/capability/fact and life-mapping fields do not constitute a Combat checkpoint. Numerical writes use controlled Stats entries; consumer replay and fixed Sim scheduling remain owner responsibilities. General allocator failure and violation of pure-extension/owner contracts are outside the retryable transaction guarantee.

## Gameplay facade and automatic integration (N0–N2)

The default installCombat(World) entry hides settlement machinery and maintains
consistency between Entity, Stats and Combat. configureActor stores one-time
configuration for current actor replacement. damage/heal use that life mapping;
increase/decrease/setAttribute reuse the Stats World service and recipe book.
Advanced APIs remain available through the same settlement chain.

- Provide a World/Host-owned gameplay facade with one-time profile/configuration.
  Routine damage/heal calls supply participating EntityHandles, amount and an
  optional configured recipe name. Bindings, incarnations, versions, calculation identity,
  tick, default plans, hit reservations and one-shot attack cleanup are internal.
  No caller must construct a SettlementRequest or manually open/close a ledger.
- Default installation assembles optional integration dependencies and connects
  their lifetimes. Advanced rules, explicit attack windows and custom plans remain
  opt-in extensions of the same implementation, not a separate numerical system.
  Missing configuration/authority/entities still return clear gameplay errors.
- Stats owns the only authoritative numerical state; Combat owns relation and
  life policy. The Entity/Host integration observes every supported authoritative
  state publication, including direct Stats commits, effect/capacity changes,
  batch publication and configured-state replacement. It updates bound Combat
  state automatically. Stats Core must not depend on Combat or know death rules.
- Internal consistency propagation is separate from user observers. It completes
  before a synchronous gameplay call reports success and before any external
  notification. Deferred public callbacks and per-frame polling alone cannot
  maintain this invariant. Internal wiring must not replace or depend on the
  application's single setOnSettled callback. Preflight storage/capacity remains
  before authoritative mutation; callback errors cannot reopen successful work.
- Definition replacement/restore and component/World recreation automatically
  renew live bindings using configured life policy, while already queued old
  requests remain stale. Automatic synchronization does not imply automatic
  revival: healing/death/revival semantics remain the selected gameplay policy.
- The facade drives its result delivery at the normal simulation boundary.
  Applications may subscribe to gameplay outcomes without manually draining a
  cross-system queue for consistency. Advanced consumers may inspect raw results.
  Each simple call represents a new action; transport retries must retain one
  command identity through the integration rather than create fresh damage calls.

Acceptance: configure a basic actor once, damage/heal it without any Stats or
ledger calls, read consistent resource/life state immediately on success, observe
death once after notification, and verify direct Stats changes, user observer
replacement, deletion/reconfiguration and World teardown need no manual sync.
Executable acceptance cases are in Test_Gameplay.cpp, Test_Scene.cpp and
Test_Host.cpp; concrete validation reports are recorded in the engine plan.
The result and life queues each have 1024 slots; internal preflight accounts for
the whole batch publication count before mutation. Mandatory publication observers
are weakly registered, pure on preflight and allocation-free/noexcept on publish.
User callbacks cannot destroy owners or reenter Combat. Simple fixed Sim delivery
clears notifications even without subscribers; advanced callers can drain both
outcomes and life transitions. Simple damage/heal return ActionResult (actual,
remaining, excess and life); generic increase/decrease/setAttribute return
AttributeResult (before/after and signed delta). Public subscriptions retain
these owned values from the completed action, independent of later changes.
Authored life mapping serializes the attribute name instead of a floating-point
representation of its 64-bit ID. Other authored life fields serialize, while recipe books,
actor profile registries and runtime phase/sequence are not checkpoints.

## Accepted follow-up: recipes, generic changes and area effects

The post-implementation discussion is consolidated in engine plan section 16.
The first milestone has delivered numerical contracts, internal publication and
simple numerical entries. Permission/effect instance and area portions below remain planned.

- Damage/heal resolve the profile's life pool. Generic operations select another
  stat and Increase/Decrease/Set intent. Persistent pools normally mutate current;
  scalar changes mutate base and rebuild effective values. Temporary boosts use
  owned modifiers rather than overwriting effective values.
- Stats defines and evaluates both effective-value rules and operation recipes.
  Integration selects the configured default by stat and operation, or an explicit
  compatible recipe, and binds source/target stats and authored parameters to pure
  input slots. Missing or incompatible configuration fails before mutation.
  Direct amount recipes provide the basic default. There is no Combat formula bank.
- Direct numerical changes use Combat increase/decrease/setAttribute, or the Stats
  gameplay facade when no combat integration is needed. Interactions needing
  generic source attribution and target permission will use the planned generic action path.
  Generic life-pool mutation updates life policy but does not invent a damage hit,
  damage event or kill attribution. General command deduplication is distinct from
  damage hit allowance. Simple and advanced entries share numerical ownership.
- Internal state publication must cover effects, current changes caused by
  capacity changes, batch publication and state replacement. Track the mapped
  pool's final current value, not every Change field as if it represented health.
  User settlement callbacks are separate from mandatory internal propagation.
  Controlled effect removal/update and scoped connections are integration work;
  the const Stats state view is never bypassed.
- Candidate discovery, selection eligibility and operation permission are separate.
  Generic changes and ApplyEffect require declared intent, relation and immunity
  policy; a signed output alone cannot determine beneficial/harmful permission.
- A reusable effect definition produces independent application instances. Each
  instance owns numerical modifier handles and any nonnumerical status/tag
  contributions. Removal releases only those contributions; overlapping stun or
  immunity must not be cleared with a global boolean reset. Refresh/replace/stack,
  input snapshot/recalculation and feedback rejection are explicit contracts.
- An area action maintains stable per-target membership on the existing fixed Sim
  boundary. By default a target must remain in range, permitted and selected;
  loss of eligibility removes the instance's contribution. Retained members are
  not reapplied each tick. Start with a headless candidate provider, then add
  optional physics providers. Candidate limits must not manufacture exits.
  Numerical timers remain Stats-owned; there is no additional numerical clock.
- Single-target composite effects need a preflight and common completion point for
  numeric and status contributions. Area targets default to independent settlement
  with per-target outcomes. Cross-object atomic mode must consume the validated
  Stats batch contract, never imitate it with serial commits. Lifecycle policies
  cover source/target destruction, death, cancellation, expiry, pause and teardown.
  Source-owned auras normally clean up on source termination; independently lasting
  effects may retain provenance after the source disappears.

First-milestone acceptance adds default/explicit recipes for mana/rage/scalar
operations and all authoritative publication paths to the simple damage/heal
cases. The second milestone proves overlapping instances and a selected-friendly
area boost through real Stats/Entity integration. Shield routing, derived
reactions and Ability/Combo execution remain later work.

## Dependency boundaries

AYCombat::Core has no Stats/Entity/Physics dependency. AYCombat::Stats links Core and AYStats::Core. Optional AYCombat::Entity links Stats adapter and AYStats::Entity; it is assembled after those targets in the root build. Standalone Entity configuration fails clearly. Root opt-in requires the local checkouts; absent Stats must disable the adapter or fail configuration clearly. No remote/submodule URL is invented.

A single StatSet transaction is not a cross-object transaction. Multi-pool shield settlement, reflection/lifesteal and batch completion must use validated Stats contracts in later stages. Geometry, animation and Combo decisions consume Combat results in higher or optional layers; Core does not know skill graphs.

## Validation

AYCombat_UnitTests partitions Core/ledger as fast and real Stats integration as integration, with disjoint full-tier coverage. AYCombat_Example builds and runs a real 30-damage/70-health scenario. Core-only compilation disables the Stats adapter and excludes its integration suite. Use the engine inventory auditor and case reports for authoritative case counts; CTest partition count is not case count.
