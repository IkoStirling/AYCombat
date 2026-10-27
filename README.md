# AYCombat

Independent Git repository on `main`, registered as an AliyatEngine submodule. Provides relationships, target permission, provenance, stable candidate selection, hit reservation/replay protection, ordered rule predicates, real single-resource Stats settlement, explicit life policy and an optional World/Entity adapter.

`AYCombat::Ballistic3D` provides `traceBallisticSurfaces3D` over the current
Jolt 3D multi-hit query. The game resolves each exact collider to an authored
surface cost and optional grazing-angle ricochet rule. The trace returns
ordered penetration, stop and ricochet steps with remaining energy; a bounce
starts a new bounded query and excludes the surface just hit. It does not
infer geometry thickness or apply damage. The game maps accepted steps to
Combat/Stats at its serialized Sim boundary. See
`AYCombat_Ballistic3DTests`.

`AYCombat/InitiativeClock.h` provides an optional game-owned fixed-tick action
order for turn-style and ATB-style combat. The host advances logical ticks,
starts one ready actor, runs its own Ability/Combat action, then finishes that
actor. Equal rates produce rounds; differing rates preserve the earliest ready
tick and stable actor-ID ties. The clock has no Entity pointers or damage rules;
its validated snapshot can be stored with the game's battle state. See
`AYCombat_InitiativeExample` and `CombatInitiativeClockTests`.

`settleResources` commits one accepted hit to up to 32 distinct persistent
Stats pools in one target StatSet. Games calculate the part/armor distribution
and pass nonnegative mutations; Combat validates direction and pool identity,
reserves one hit receipt, then publishes all resource changes or none. The
outcome retains every Stats change and optional life transition. See
`AYCombat_ArmorHealthExample` and `CombatStatsIntegrationTests`.
For a common one-part armor route, `planPartArmorHit` previews a deterministic
split and `settlePartArmorHit` publishes it through the same hit receipt and
atomic Stats commit. The game maps the collider to a part and supplies a
0..10000 absorption rate; armor absorbs at most its current pool, with the
remainder applied to health. The helper does not infer penetration geometry,
coverage probability, equipment durability or medical rules.
`settleBatch` stages 1..64 ordered hits against up to 64 caller-owned
`StatSet` targets, optional life trackers and a copied HitLedger. It publishes
all numerical states and receipts only after every hit succeeds. Repeated
hits on one target use the live version at batch start and advance staged
versions in caller order. The game owns action state, Entity notifications,
World/Inventory effects and persistent save. This is a low-level Stats entry;
the World-owned `CombatSystem::submit` still settles one target per call.

## Build and validate

From the engine root on the current Windows development host:

```powershell
pwsh -NoProfile -File AYRuntime/AYCombat/tools/build-local.ps1 -Configure
pwsh -NoProfile -File scripts/tests/run-module-tests.ps1 -Module AYCombat -Tier full -BuildDirectory out/build/combat-core -SkipBuild -Audit -ReportDirectory out/combat-reports
& ./out/build/combat-core/AYCombat_Example.exe
```

The helper accepts `-VsRoot` and `-ExtraConfigureArguments`. Portable CMake callers can configure this directory directly with a C++23 compiler and the AYTest dependency prerequisites. Core-only build:

```powershell
pwsh -NoProfile -File AYRuntime/AYCombat/tools/build-local.ps1 -Configure -BuildDirectory out/build/combat-core-only -Targets AYCombat_UnitTests -ExtraConfigureArguments '-DAY_COMBAT_BUILD_STATS=OFF'
```

In the root engine build enable `AY_ENABLE_COMBAT` and `AY_ENABLE_STATS` (both default OFF). Link `AYCombat::Stats` for the recommended `settle` facade, or `AYCombat::Core` for pure Core. Disable `AY_COMBAT_BUILD_STATS` when no numerical adapter is wanted.

Enable `AY_COMBAT_BUILD_ENTITY=ON` in a root engine build to link `AYCombat::Entity`. Before registry seal call `registerCombatGameplayComponents`, then `installCombat(world)` acquires the World-owned gameplay facade and installs its numerical dependencies. Configure each actor once with `ActorProfile` (Stats schema, mapped life pool, life policy and team), then configure explicit directional team relations. Different teams are not implicitly hostile.

`configureActor` initializes or deliberately rebuilds an actor's Stats from its schema. For an Entity that already has configured authoritative Stats, call `attachActor(handle, lifePolicy, team)` instead. It validates the existing persistent life pool, adds Combat if needed, and preserves values, effects, version, generation and pending commands.

For a reproducible pairing, use AYStats `main` at `602c57c` or newer; its combat-compatible source baseline is `6ea15f80dde6d154eea1ab1873d55e2d7ae51ccc`. The root repository records compatible AYCombat and AYStats commits as submodule pointers.

```cpp
auto& combat = *installCombat(world).value(); // Check Result in production; see Gameplay.cpp.
// configureActor(source, profile) and configureActor(target, profile) once.
auto damage = combat.damage(source, target, 30);
auto heal = combat.heal(target, target, 10);
auto cost = combat.decrease(source, "mana", 20);
auto alternateCost = combat.decrease(source, "mana", 20, "skill_cost");
// Existing Stats state: combat.attachActor(source, {statId("vitality")}, 1);
```

Routine calls need no calculation plan, input span, expected version, binding or ledger management. Damage/heal return `ActionResult` with non-negative actual amount, remaining pool value, excess and optional life transition. Generic `increase/decrease/setAttribute` return before/after and signed actual delta in `AttributeResult`; they change pool current or scalar base without inventing damage permission/attribution. Configure a Stats `RecipeBook` once with `attributes().setRecipes`: defaults are indexed by target stat and Increase/Decrease/Set, explicit compatible names override them, and missing defaults use direct amount. Inputs sample current source/target state or authored constants; pure formulas remain in Stats. Source-bound generic recipes use `changeAttribute` with an explicit source. Recipe metadata is runtime configuration owned by Stats.

`AYCombat/Action2DIntegration.h` offers `dispatchActionHits2D` for ordinary `ActionRuntime2D` results. It resolves each stable action target ID to a current EntityHandle, calls the World-owned Combat facade, and returns one ordered settlement per hit, including an explicit missing-target result. Call it exactly once for each completed action step; `ActionRuntime2D` deduplicates targets within each authored hit window. When a caller may retry delivery, keep an `Action2DCombatReceipts` beside that World and call `settle` instead. It retains bounded per-source/cycle/window/target results, including failures, so an identical retry cannot apply damage again while a newly authored window may hit again; changed damage, recipe or hit box is rejected. Call `forgetBefore` only after old cycles can no longer be retried and `clear` on World replacement or restore. Neither route is a deterministic Session/checkpoint adapter or a multi-target atomic transaction.
`AYCombat/Action2DBatch.h` provides separate low-level `stepAndSettleActionPath2D` and `stepAndSettleActionGroup2D` routes when ordinary action state and 1..64 ordered Combat hits must publish together. The caller gives the actual paths/targets and optional static blockers, then a pure mapper turns each resulting hit into a borrowed `CombatBatchHit` with the matching tick. For multiple authored windows within one action cycle, open the attack with `HitScope::Segment` and map `ActionStep2D::hitWindow` to `HitRequest::segment`; request IDs must still be unique. The group route stages 1..64 actor runtimes, visiting sources in ascending actor ID order and each source's hit order. The adapters stage actions before `settleBatch`; a failed mapping or Combat hit leaves every action clock, target StatSet, life tracker, HitLedger and output unchanged. Hitless actions advance without Combat. The game still owns action-target to Combatant identity mapping, World/Entity notifications, and persistence. Root builds with AY2D enabled link these header-only routes through `AYCombat::Stats` plus `AY2D`; standalone Core/Stats builds need no AY2D.
For one World-owned, durable 2D tick, link `AYCombat::Host` and use `Action2DWorldCommit.h`. `commitActionGroupWorld2D` stages the action runtimes, Stats, life trackers, HitLedger, a fingerprinted WorldStateLedger receipt and outcome notices in DurableOutbox. The game gives stable request identity, a read-only Entity/component generation preflight, and a failure-atomic save callback. `encodeAction2DWorldCandidate` supplies a bounded versioned JSON section for the same SaveGame slot as the other game state. After success, delivery uses `dispatchPendingOutbox`; the receiver deduplicates the notice key. `decodeAction2DWorldSnapshot` validates Stats schemas and HitLedger shape before the game restores each ActionRuntime2D against its authored library and publishes a staged World. The game still owns physical paths, target binding and Entity effects. See `Test_Action2DWorldCommit.cpp` for save rejection, two attackers causing one death, restart and notification retry.
这两个联合结算入口还可选接收 `ActionMovingBlocker2D`：游戏提供移动门或掩体在同 tick 的实际路径，动作候选先裁去障碍后的命中，再与数值批次一同提交。旧的仅静态障碍调用方式继续可用；非法移动路径会在提交前拒绝。
`AYCombat/Action3DBatch.h` 的可选 `stepAndSettleFixedAction3D` 把一个 `FixedActionTimeline` tick 与最多 64 个低层 Combat 命中放在同一发布边界。游戏在活动命中窗内进行只读物理查询，并把每个请求的 tick、Ability execution 和 segment 分别对齐当前帧、执行实例与窗口 ID。映射或结算失败会恢复时间线 checkpoint，不会推进生命、数值、命中账本或输出。需要同时链接 `AYAbility::Core` 和 `AYCombat::Stats`；根运动、Entity 通知与持久化仍由游戏编排。

需要跨重启防止重复命中时，可链接 `AYCombat::Host` 使用 `AYCombat/Action3DWorldCommit.h` 的 `commitActionWorld3D`。它在私有时间线和 Combat 值状态上结算，将指纹收据及 Combat 结果通知加入 WorldStateLedger/DurableOutbox 候选，调用游戏的失败原子同槽保存回调后才发布。版本 1、有界 JSON 节包含时间线 checkpoint、目标 Stats/生命、HitLedger、收据及 outbox；游戏恢复时提供原动作定义和 Stats schema，并把物理姿态、Entity 身份和其他 World 节放在同一槽。外部通知在保存后用 outbox 重试，接收方按 key 去重。预览后仍须传入预览前 checkpoint；游戏负责只读命中查询、实际根运动与代次 preflight。

2D/3D 批次都可传入 `ActionCombatPreflight`，在命中查询和映射完成后、数值
发布前复核本 tick 捕获的 World/Entity/组件代次；失败保持动作、数值和命中
账本不变。World 适配器的 `validateCombatantBinding` 对捕获的
`EntityBinding` 做只读校验。宿主须序列化这一模拟边界，且仍负责通知与存档。
若先用 `previewStep` 准备动画和物理，提交时传入预览前保存的 `FixedActionSnapshot`；中途发生时间线更改时适配器返回 Stale，且不会读取命中或发布状态。不预览的调用方可使用省略预期快照的便利重载。

Advanced `CombatOutcome` retains the request's part and segment IDs, including denied results. A physics adapter can provide exact collider-to-part mapping so Part-scope hit deduplication distinguishes multiple shapes on one target.

An authored `EffectDefinition::lifeRestore` can request `RestoreSnapshot` or `RestoreNetDelta` for a temporary mapped-life-capacity modifier. The default `KeepCurrent` retains normal Stats capacity-policy behavior. Exact modes capture life current before/after application, keep the numerical modifier alive until Combat removes it, and atomically remove the modifier with the configured current restoration. `RestoreNetDelta` adds the net current change during the effect to the original current, then clamps to the restored capacity. Only one exact-restoration effect may be active on a target; replacing such an effect is rejected, while refresh is supported.

For a source-aware change that needs target permission, author an `InteractionPolicy` and call `interactAttribute(source, target, stat, operation, amount, policy, selected?, recipe?)`. The policy declares Beneficial/Harmful/Neutral intent, allowed relations, optional required tag, selection and effect type; intent never comes from the sign of a value. Relation lookup uses current teams by default, or a caller-captured `relationSnapshot` when configured. `applyEffect(source, target, effect, stackPolicy, policy, selected?)` applies a Stats numerical effect only after the same permission check. The target's `CombatComponent` supplies current base `tags` and `immuneEffects`; the caller supplies the current selection fact. An ordinary denial returns `InteractionResult.decision` without a Stats write; a successful result carries an owned source snapshot and attribute result or Stats effect ID. These are single-target synchronous entry points. Simple `increase/decrease/setAttribute` remain direct numerical operations without an implicit permission gate.

For a managed lifetime, use `applyOwnedEffect(source, target, EffectDefinition, policy, selected?)`. Combat allocates a distinct instance and modifier handles, owns its source/target incarnations and contributes tags, effect immunity, damage-type immunity and stun without overwriting the target's base facts. Stats owns any numerical modifiers and their tick-based expiry. `refreshOwnedEffect` renews the same instance; `replaceOwnedEffect` atomically changes that instance's numeric/status contributions while retaining its type/key/stack policy; `removeOwnedEffect` removes only that instance. `removeSourceEffects` cancels a source's instances, and fixed Sim cleans up expired, destroyed-source and replaced-component instances. Multiple same-key sources remain independent up to the authored `maxStacks`. Effective facts are available through `hasTag`, `immuneToEffect`, `immuneToDamageType` and `stunned`; owned stun prevents that actor from initiating simple damage/heal and permissioned interactions. Advanced submit retains caller-supplied source snapshots and requires its host to enforce source-action restrictions. A failed application/replacement leaves prior state intact. The earlier `applyEffect` remains a lower-level Stats-ID operation whose lifecycle the caller manages.

For a range or selected-target boost, `startArea(source, AreaDefinition, provider)` accepts a callback that returns the complete current in-range candidate set with each candidate's selection fact. At every fixed Sim step Combat sorts/deduplicates candidates, rechecks current relation, selection, tags, immunity and life, then applies an N4 instance on entry, keeps its ID while eligible and removes it on exit. `stopArea` cancels explicitly; source death/destruction cancels automatically, and `removeSourceEffects` also stops that source's areas. `lastAreaSweep` exposes per-target entered/retained/exited/rejected/failed results for a live area; optional `setOnAreaSweep` receives fixed-Sim sweeps including automatically ended areas. Candidate/member limits reject a whole sweep before mutation, retaining existing members. The provider must return an untruncated snapshot and must not mutate the World or Combat during enumeration. A permanent `EffectDefinition` is the normal aura choice; finite durations can expire and be reapplied while membership remains eligible. Presentation updates do not advance this lifecycle. Optional `AYCombat::Physics2D` supplies `makeAreaCandidateProvider2D` for source-following Box2D circles: the source has a Transform, targets have current 2D RigidBody bindings, and an optional callback supplies current selection. World and PhysicsManager must outlive the area. Optional `AYCombat::Physics3D` supplies source-following Jolt sphere candidates through the same Combat membership rules.

`CombatIntegrationModule` registers/installs the numerical dependencies automatically. Optional `AYCombat::Scene`/`AYCombat::Host` and `CombatHostIntegrationModule` connect current/edit/play World creation through the Host, without adding separate Stats module nodes. Reacquire World-owned facade references after World replacement. Authored life mapping serializes by attribute name so 64-bit IDs do not lose precision through the reflection serializer; runtime life/immuneTypes/queues and recipe/profile registries are not a Combat checkpoint.

After an Actor or Scene is composed, `bindCombatant` makes authored Stats definitions and the Combat life mapping ready on first use, before the first fixed Sim tick. Gameplay does not need to call component `onStart` or manually synchronize these owners.

Subscribe with `setOnOutcome`/`setOnLifeChanged`; simple installation delivers and clears bounded notifications on fixed Sim, even without subscribers. Internal life propagation completes synchronously before success/user callbacks and covers direct Stats commits, effects, capacity clamps, batches, reconfiguration and restore. Replacing public Stats callbacks does not disconnect it. Default life policy does not revive through healing. Callback failures are post-commit diagnostics; callbacks cannot destroy their owners or reenter Combat. Queue limits reject before mutation.

Advanced users may still `bindCombatant`, open explicit windows and `submit` through the same settlement chain; its numerical observer delivery remains NextTick. `takeOutcomes`/`takeLifeEvents` support explicit advanced delivery, and current/queued bindings retain generation checks. Each simple call creates a new action; transport/command retries must retain the advanced identity. Core rules own no numerical status timers.

See [the gameplay example](examples/Gameplay.cpp) (`AYCombat_GameplayExample`), [advanced settlement](examples/SingleTarget.cpp), [interaction checks](unittest/Test_Interaction.cpp), [gameplay scenarios](unittest/Test_Gameplay.cpp), [contracts](design.md) and [execution plan](../../AYDocs/AYCOMBAT-IMPLEMENTATION-PLAN.md). Core stores no Entity pointers. Every Result must be checked. Full CombatSystem effects/area runtime save, shields, reactions and Ability/Combo execution remain planned; the optional 2D value-batch snapshot covers only its explicit owners.

## Editor diagnostic observations (2026-10-15)

CombatSystem::debugAreas returns owned source/last-sweep observations without sampling providers, changing membership or replacing callbacks. The optional Physics2D area provider records its exact tested region through the manager diagnostic service. The editor marks affected and rejected/exited/failed area targets from real sweep results.
Capture follows the shared debug lifetime, freeze and budget; disabled capture
has no retained history. Diagnostics are not checkpointed or serialized.

Verification: audited full inventory 94/94 passed, including owned area
observation lifetime. Report:
`out/debug-visualization-20261015/combat-full/e56ba939bed34c13b153b188667da35b/cases.json`.

## Debug content extension (2026-10-15)

Optional owner-thread debug capture defaults off. debugOutcomes and debugInteractions retain independent128-event histories with sequence, fixed tick and monotonic completion time; gameplay takeOutcomes/automatic callbacks remain independent. Debug allocation failures cannot change committed results. debugEffects copies up to512 current owned effects and remaining fixed ticks; UINT64_MAX means permanent. Clear/disable drops event observations, not effects or gameplay state.

Module-local regression sources were updated. Runtime tests and interactive editor acceptance are deferred by the current task request; previous validation reports cover the earlier baseline. These changes remain uncommitted until focused runtime checks are requested and pass.

## 3D area candidates (2026-10-24)

`AYCombat::Physics3D` exposes `makeAreaCandidateProvider3D` for source-following
Jolt spheres and oriented boxes. A current source Transform defines the center
and box orientation; the query maps
overlapping 3D rigid bodies to unique Entity handles with Stats and Combat.
Scenery is ignored, duplicate body ownership is rejected, and Combat applies
selection, relation, immunity and membership rules on every fixed Sim sweep.
The World and PhysicsManager must outlive active areas; query failures preserve
prior Combat membership through the existing sweep contract. This adapter
does not change the headless candidate provider. The offset remains in world
coordinates; only a box's orientation follows the source rotation.
