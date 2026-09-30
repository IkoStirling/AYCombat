# AYCombat

Independent local Git repository on `codex/aycombat-core`, with no remote. Provides relationships, target permission, provenance, stable candidate selection, hit reservation/replay protection, ordered rule predicates, real single-resource Stats settlement, explicit life policy and an optional World/Entity adapter.

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

In the root engine build enable `AY_ENABLE_COMBAT` and `AY_ENABLE_STATS` (both default OFF for local unpublished modules). Link `AYCombat::Stats` for the recommended `settle` facade, or `AYCombat::Core` for pure Core. Disable `AY_COMBAT_BUILD_STATS` when no numerical adapter is wanted.

Enable `AY_COMBAT_BUILD_ENTITY=ON` in a root engine build to link `AYCombat::Entity`. Before registry seal call `registerCombatGameplayComponents`, then `installCombat(world)` acquires the World-owned gameplay facade and installs its numerical dependencies. Configure each actor once with `ActorProfile` (Stats schema, mapped life pool, life policy and team), then configure explicit directional team relations. Different teams are not implicitly hostile.

`configureActor` initializes or deliberately rebuilds an actor's Stats from its schema. For an Entity that already has configured authoritative Stats, call `attachActor(handle, lifePolicy, team)` instead. It validates the existing persistent life pool, adds Combat if needed, and preserves values, effects, version, generation and pending commands.

For a reproducible local pairing, use AYStats `main` at `602c57c` or newer; its combat-compatible source baseline is `6ea15f80dde6d154eea1ab1873d55e2d7ae51ccc`. The `codex/combat-compatible-stats` branch retains that baseline for traceability. Both repositories remain local with no remote.

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

For a source-aware change that needs target permission, author an `InteractionPolicy` and call `interactAttribute(source, target, stat, operation, amount, policy, selected?, recipe?)`. The policy declares Beneficial/Harmful/Neutral intent, allowed relations, optional required tag, selection and effect type; intent never comes from the sign of a value. Relation lookup uses current teams by default, or a caller-captured `relationSnapshot` when configured. `applyEffect(source, target, effect, stackPolicy, policy, selected?)` applies a Stats numerical effect only after the same permission check. The target's `CombatComponent` supplies current base `tags` and `immuneEffects`; the caller supplies the current selection fact. An ordinary denial returns `InteractionResult.decision` without a Stats write; a successful result carries an owned source snapshot and attribute result or Stats effect ID. These are single-target synchronous entry points. Simple `increase/decrease/setAttribute` remain direct numerical operations without an implicit permission gate.

`CombatIntegrationModule` registers/installs the numerical dependencies automatically. Optional `AYCombat::Scene`/`AYCombat::Host` and `CombatHostIntegrationModule` connect current/edit/play World creation through the Host, without adding separate Stats module nodes. Reacquire World-owned facade references after World replacement. Authored life mapping serializes by attribute name so 64-bit IDs do not lose precision through the reflection serializer; runtime life/immuneTypes/queues and recipe/profile registries are not a Combat checkpoint.

Subscribe with `setOnOutcome`/`setOnLifeChanged`; simple installation delivers and clears bounded notifications on fixed Sim, even without subscribers. Internal life propagation completes synchronously before success/user callbacks and covers direct Stats commits, effects, capacity clamps, batches, reconfiguration and restore. Replacing public Stats callbacks does not disconnect it. Default life policy does not revive through healing. Callback failures are post-commit diagnostics; callbacks cannot destroy their owners or reenter Combat. Queue limits reject before mutation.

Advanced users may still `bindCombatant`, open explicit windows and `submit` through the same settlement chain; its numerical observer delivery remains NextTick. `takeOutcomes`/`takeLifeEvents` support explicit advanced delivery, and current/queued bindings retain generation checks. Each simple call creates a new action; transport/command retries must retain the advanced identity. Core rules own no numerical status timers.

See [the gameplay example](examples/Gameplay.cpp) (`AYCombat_GameplayExample`), [advanced settlement](examples/SingleTarget.cpp), [interaction checks](unittest/Test_Interaction.cpp), [gameplay scenarios](unittest/Test_Gameplay.cpp), [contracts](design.md) and [execution plan](../../AYDocs/AYCOMBAT-IMPLEMENTATION-PLAN.md). Core stores no Entity pointers. Every Result must be checked. Combat-owned effect instances and contributions, area membership, shields, physics mapping, reactions, runtime save/load and Ability/Combo execution remain planned.
