[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out/build/combat-core',
    [string]$Configuration = 'Release',
    [string[]]$Targets = @('AYCombat_UnitTests', 'AYCombat_Example'),
    [switch]$Configure,
    [string]$SourceDirectory = 'AYRuntime/AYCombat',
    [string]$VsRoot = 'E:/MySoftware/vs2026insider',
    [string[]]$ExtraConfigureArguments = @()
)
$ErrorActionPreference = 'Stop'
$engineRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$combatCompilerRoot = Get-ChildItem -LiteralPath "$VsRoot/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
$combatCompilerRoot = $combatCompilerRoot.Replace('\', '/')
$combatSdkRoot = (Get-ItemProperty 'HKLM:/SOFTWARE/Microsoft/Windows Kits/Installed Roots').KitsRoot10
$combatSdkRoot = $combatSdkRoot.Replace('\', '/')
$combatSdkVersion = Get-ChildItem -LiteralPath (Join-Path $combatSdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
$env:INCLUDE = "$combatCompilerRoot/include;$combatSdkRoot/Include/$combatSdkVersion/ucrt;$combatSdkRoot/Include/$combatSdkVersion/shared;$combatSdkRoot/Include/$combatSdkVersion/um;$combatSdkRoot/Include/$combatSdkVersion/winrt"
$env:LIB = "$combatCompilerRoot/lib/x64;$combatSdkRoot/Lib/$combatSdkVersion/ucrt/x64;$combatSdkRoot/Lib/$combatSdkVersion/um/x64"
$env:PATH = "$combatCompilerRoot/bin/Hostx64/x64;$combatSdkRoot/bin/$combatSdkVersion/x64;$VsRoot/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja;$env:PATH"
$env:VSLANG = '1033'
Push-Location $engineRoot
try {
    if ($Configure) {
        & cmake -S $SourceDirectory -B $BuildDirectory -G Ninja "-DCMAKE_CXX_COMPILER=$combatCompilerRoot/bin/Hostx64/x64/cl.exe" "-DCMAKE_MT=$combatSdkRoot/bin/$combatSdkVersion/x64/mt.exe" "-DCMAKE_RC_COMPILER=$combatSdkRoot/bin/$combatSdkVersion/x64/rc.exe" "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_PREFIX_PATH=$engineRoot/out/build/vcpkg_installed/x64-windows" @ExtraConfigureArguments
        if ($LASTEXITCODE -ne 0) { throw 'AYCombat configure failed' }
    }
    & cmake --build $BuildDirectory --target @Targets --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'AYCombat build failed' }
} finally { Pop-Location }
