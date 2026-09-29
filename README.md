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

Enable `AY_COMBAT_BUILD_ENTITY=ON` in a root engine build to link `AYCombat::Entity` and build `AYCombat_EntityTests`. This requires AYStats Batch/Assets/Entity integration. Register Stats/Combat components before sealing ComponentRegistry; install StatsSystem then CombatSystem in the selected World. Configure Stats, configure an explicit life resource policy, and call `bindCombatant` with the EntityHandle. Set relationships, open an attack, and call `CombatSystem::submit`; its adapter uses `StatsComponent::commit(..., NotificationDelivery::NextTick)` and verifies World/Entity/component generations.

`takeOutcomes()` transfers owned results after settlement. Consumers can choose `Blocked`, `Zero`, `Damage`, `Heal` or a life transition for hit feedback/Ability/Combo confirmation. Queue capacity is 1024; drain at an owner-controlled point. Stats observers run on the next fixed Stats tick, after Combat completion. Rules consume current facts but own no status timers. Default life policy does not revive through healing; choose this explicitly when configuring the policy. External Stats writes require the owner to synchronize LifeTracker separately.

See [the runnable example](examples/SingleTarget.cpp), [real Entity scenarios](unittest/Test_EntityIntegration.cpp), [contracts](design.md) and [execution plan](../../AYDocs/AYCOMBAT-IMPLEMENTATION-PLAN.md). Core stores no Entity pointers. Every Result must be checked. Physics mapping, reactions, runtime save/load and Ability/Combo execution remain planned.
