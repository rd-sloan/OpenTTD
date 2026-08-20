<#
.SYNOPSIS
    Runs a fixed-work OpenTTD benchmark from a savegame and records a report.

.DESCRIPTION
    Phase 0 harness for the EnTT migration (see docs/ecs-migration-plan.md).

    Simulates a fixed number of ticks headlessly, then writes a tab separated report
    of timings, workload counts and object sizes to benchmark/out. Optionally runs
    twice and compares the resulting exit saves byte for byte, which is the
    determinism check every migration phase has to pass.

    This script exists because several details of running OpenTTD headlessly on
    Windows are non-obvious and easy to get wrong:

    - The game is a GUI subsystem binary. CreateConsole allocates a fresh console and
      reopens the standard streams onto it, so stdout and stderr never reach a
      redirecting shell. All output therefore goes to files.
    - Passing -c <path> makes the config file's own directory a data search
      directory, so a config outside the build tree hides build/baseset and the game
      exits with no graphics set. The config is copied into the build directory here
      to avoid that.
    - gui.autosave_on_exit defaults to false, so without bench.cfg no exit save is
      produced and a determinism check would silently compare nothing.

.PARAMETER Save
    Savegame name under benchmark/saves, with or without the .sav extension.

.PARAMETER Ticks
    Number of game ticks to simulate.

.PARAMETER Label
    Label for the output files, e.g. a phase name. Defaults to 'run'.

.PARAMETER Config
    Build configuration to run: Debug or RelWithDebInfo. Defaults to RelWithDebInfo,
    because timings from a build with asserts enabled are not worth recording.

.PARAMETER BuildDir
    Build tree to run from, relative to the repository root. Defaults to 'build-release',
    which is configured with OPTION_USE_ASSERTS=OFF and is the tree to take timings from.
    Use 'build' for the assert-enabled tree, which is the one to use for correctness work
    such as shadow-mode assertions.

    The two trees produce separate report files, because their timings are not comparable.
    Fingerprints are comparable across both, since asserts do not change game logic.

.PARAMETER CheckDeterminism
    Run twice and compare the game state fingerprints. Note that savegames are NOT
    compared byte for byte, because they are not a pure function of the game state;
    see the README.

.PARAMETER CompareTo
    Path to an earlier report to compare fingerprints against. This is the phase gate:
    a migration step that was meant to preserve behaviour must produce the same
    fingerprint as the phase 0 baseline.

.EXAMPLE
    .\run-benchmark.ps1 -Save Hilbergen -Ticks 20000 -Label phase0

.EXAMPLE
    .\run-benchmark.ps1 -Save wentbourne -Ticks 5000 -Label phase0 -CheckDeterminism
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Save,
    [int]$Ticks = 20000,
    [string]$Label = 'run',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')][string]$Config = 'RelWithDebInfo',
    [string]$BuildDir = 'build-release',
    [switch]$CheckDeterminism,
    [string]$CompareTo
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repoRoot $BuildDir
$exe = Join-Path $buildPath "$Config\openttd.exe"
$benchDir = $PSScriptRoot
$outDir = Join-Path $benchDir 'out'

if (-not $Save.EndsWith('.sav')) { $Save = "$Save.sav" }
$savePath = Join-Path $benchDir "saves\$Save"
$saveName = [System.IO.Path]::GetFileNameWithoutExtension($Save)

if (-not (Test-Path $exe)) {
    throw "No $Config binary at $exe. Build it first: cmake --build $BuildDir --config $Config --target openttd"
}
if (-not (Test-Path $savePath)) {
    throw "Savegame not found: $savePath"
}
if (-not (Test-Path (Join-Path $buildPath 'baseset'))) {
    throw "No baseset in $buildPath. The game needs a graphics set to start."
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# The config must live inside the build directory, or its own directory becomes a
# data search directory and the baseset is no longer found.
$activeConfig = Join-Path $buildPath 'bench.cfg'
Copy-Item -Path (Join-Path $benchDir 'bench.cfg') -Destination $activeConfig -Force

$exitSave = Join-Path $buildPath 'save\autosave\exit.sav'

function Invoke-Run {
    param([string]$StatsPath, [string]$SaveCopyPath)

    if (Test-Path $exitSave) { Remove-Item $exitSave -Force }

    $arguments = @(
        '-x'                                    # never write config changes
        '-Q'                                    # skip NewGRF scanning
        '-c', $activeConfig
        '-snull', '-mnull'                      # no audio
        "-vnull:ticks=$Ticks,stats=$StatsPath"
        '-g', $savePath
    )

    Write-Host "  running $Ticks ticks..." -NoNewline
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $buildPath -Wait -PassThru -NoNewWindow
    $sw.Stop()
    Write-Host " done in $([math]::Round($sw.Elapsed.TotalSeconds, 1))s (exit $($proc.ExitCode))"

    if ($proc.ExitCode -ne 0) {
        throw "OpenTTD exited with code $($proc.ExitCode). A non-zero exit with no output usually means it could not find the baseset or the savegame."
    }
    if (-not (Test-Path $StatsPath)) {
        throw "No report was written to $StatsPath."
    }

    if ($SaveCopyPath) {
        if (-not (Test-Path $exitSave)) {
            throw "No exit save at $exitSave. Check that bench.cfg sets gui.autosave_on_exit."
        }
        Copy-Item -Path $exitSave -Destination $SaveCopyPath -Force
    }
}

# The build tree is part of the name: two trees with different assert settings produce
# timings that must not be confused, and silently overwriting a baseline is worse than a
# verbose filename.
$stem = "$Label-$saveName-$Config-$BuildDir"
$statsPath = Join-Path $outDir "$stem.tsv"

Write-Host "Benchmark: $saveName / $Config / $BuildDir / $Ticks ticks"
Invoke-Run -StatsPath $statsPath -SaveCopyPath (Join-Path $outDir "$stem-a.sav")

# Read the state.* keys of a report into a hashtable.
function Get-Fingerprint {
    param([string]$Path)

    $fp = @{}
    foreach ($line in Get-Content $Path) {
        if ($line -match '^(state\.[^\t]+)\t(.+)$') { $fp[$Matches[1]] = $Matches[2] }
    }
    return $fp
}

# Compare two fingerprints and report which subsystems differ. The per-subsystem split
# is the point: it turns "something changed" into "vehicles changed, towns did not".
function Compare-Fingerprint {
    param([hashtable]$Expected, [hashtable]$Actual, [string]$What)

    if ($Expected.Count -eq 0 -or $Actual.Count -eq 0) {
        Write-Host "  SKIP: $What has no fingerprint (report predates the fingerprint?)" -ForegroundColor Yellow
        return $null
    }

    $differing = @()
    foreach ($key in ($Expected.Keys | Sort-Object)) {
        if ($Actual.ContainsKey($key) -and $Expected[$key] -ne $Actual[$key]) { $differing += $key }
    }

    if ($differing.Count -eq 0) {
        Write-Host "  PASS: $What identical (combined $($Actual['state.hash.combined']))" -ForegroundColor Green
        return $true
    }

    Write-Host "  FAIL: $What differs in $($differing.Count) of $($Expected.Count) values" -ForegroundColor Red
    foreach ($key in $differing) {
        Write-Host "    $key" -ForegroundColor Red
        Write-Host "      expected $($Expected[$key])"
        Write-Host "      actual   $($Actual[$key])"
    }
    return $false
}

$fpA = Get-Fingerprint $statsPath

# Timings from an assert-enabled build are not worth recording, so say so rather than
# letting the number look authoritative.
$asserts = (Select-String -Path $statsPath -Pattern '^run\.asserts_enabled\t(\d)').Matches.Groups[1].Value
if ($asserts -eq '1' -and $Config -ne 'Debug') {
    Write-Host "  NOTE: this build has asserts enabled, so treat the timings as indicative only." -ForegroundColor Yellow
    Write-Host "        For timings use -BuildDir build-release, which is configured with OPTION_USE_ASSERTS=OFF."
}

# Recorded for reference only. Do NOT gate on this: two identical runs of unmodified
# master produce savegames that differ, so the bytes are not a behaviour signal.
$saveHash = (Get-FileHash -Algorithm SHA256 -Path (Join-Path $outDir "$stem-a.sav")).Hash
Add-Content -Path $statsPath -Value "info.exit_save_sha256`t$saveHash"

if ($CheckDeterminism) {
    Write-Host "Determinism check: second identical run"
    $repeatPath = Join-Path $outDir "$stem-repeat.tsv"
    Invoke-Run -StatsPath $repeatPath -SaveCopyPath (Join-Path $outDir "$stem-b.sav")

    $result = Compare-Fingerprint -Expected $fpA -Actual (Get-Fingerprint $repeatPath) -What 'fingerprints'
    Add-Content -Path $statsPath -Value "determinism.reproducible`t$(if ($result) { 1 } else { 0 })"
}

if ($CompareTo) {
    if (-not (Test-Path $CompareTo)) { throw "Baseline report not found: $CompareTo" }
    Write-Host "Comparing against baseline: $CompareTo"
    Compare-Fingerprint -Expected (Get-Fingerprint $CompareTo) -Actual $fpA -What 'fingerprint vs baseline' | Out-Null
}

# Surface the figures that matter for the migration.
Write-Host ""
Write-Host "Report: $statsPath"
Select-String -Path $statsPath -Pattern '^(run\.|load\.vehicle_parts|sizeof\.Vehicle|sizeof\.Train|state\.hash\.combined|perf\.(game_loop|trains|road_vehicles|ships|aircraft|all_vehicles)\.(total_ms|pct_of_game_loop|ns_per_object_tick))' |
    ForEach-Object { "  " + $_.Line }
