# AYCombat

Independent local Git repository on `codex/aycombat-core`, with no remote. Initial implementation covers relationships, target permission, provenance, stable candidate selection, hit reservation/replay protection and real synchronous single-resource Stats settlement.

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

See [the runnable example](examples/SingleTarget.cpp), [contracts](design.md) and [execution plan](../../AYDocs/AYCOMBAT-IMPLEMENTATION-PLAN.md). The host owns and validates game-object/StatSet bindings; no Entity pointers enter Core. Every Result must be checked. This is not yet Entity/physics integration, death processing, a reaction runtime or a skill/combo system.
