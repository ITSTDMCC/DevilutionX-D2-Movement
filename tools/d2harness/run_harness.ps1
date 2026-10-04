# Part of DevilutionX-D2-Movement. Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
# Licensed under the Sustainable Use License (LICENSE.md); see NOTICE-D2MOVEMENT.md.
<#
.SYNOPSIS
  Non-visual test harness for the Diablo 2 movement mod. Runs the headless parity/collision tests,
  replays the stock demo in both movement modes with probes on, benchmarks FPS against a stock
  1.5.3 build, then filters everything into pass/fail (filter_probes.py).

.PARAMETER BuildDir      Mod release build, the exe that ships (default: build-d2parity).
.PARAMETER TestBuildDir  Mod build with the tests (default: build-d2parity-tests; MSVC test builds have no LTO).
.PARAMETER DataDir       Folder with your own DIABDAT.MPQ and devilutionx.mpq (setting DataDir). Required.
.PARAMETER D2CommonDll   Your Diablo 2 1.12 D2Common.dll, the movement reference (setting D2CommonDll). Without it the
                         two table checks are skipped and reported as failures.
.PARAMETER StockExe      Pristine 1.5.3 exe for the FPS baseline (setting StockExe; build one with
                         build.ps1 -SourceDir <git archive of tag 1.5.3>). Required when BenchRuns > 0.

Settings not given as parameters come from environment variables D2H_<Name> or tools\d2harness\local.psd1.
.PARAMETER BenchRuns     Timedemo runs per build for the FPS benchmark (best run counts). 0 skips the benchmark.
.PARAMETER MaxMinutes    Hard limit for the whole script (default 60).

Exit codes: 0 PASS, 1 FAIL, 3 MISSING_PREREQ, 4 CRASH, 124 TIMEOUT. Per-step limits are listed in README.md.
Output: tools\d2harness\runs\<timestamp>\ (summary.md, results.json, logs).
#>
param(
	[string]$BuildDir,
	[string]$TestBuildDir,
	[string]$StockExe,
	[string]$DataDir,
	[string]$D2CommonDll,
	[int]$BenchRuns = 2,
	[double]$MaxMinutes = 60
)
. (Join-Path $PSScriptRoot 'common.ps1')
Start-Watchdog $MaxMinutes

if (-not $BuildDir) { $BuildDir = Join-Path $Script:RepoDir 'build-d2parity' }
$StockExe = Get-Setting 'StockExe' $StockExe
$DataDir = Get-Setting 'DataDir' $DataDir
$D2CommonDll = Get-Setting 'D2CommonDll' $D2CommonDll
if (-not $DataDir) { Write-Step 'DataDir not set: pass -DataDir, set D2H_DataDir or add it to local.psd1'; Stop-Watchdog; exit $Script:ExitMissingPrereq }
if (-not $TestBuildDir) { $TestBuildDir = Join-Path $Script:RepoDir 'build-d2parity-tests' }
$bin = Join-Path $TestBuildDir 'Release'
$modExe = Join-Path $BuildDir 'Release\devilutionx.exe'
$python = (Get-Command python -ErrorAction SilentlyContinue).Source

$missing = @()
if (-not (Test-Path $modExe)) { $missing += $modExe }
foreach ($f in 'd2harness_test.exe', 'd2demo_test.exe', 'd2mod_test.exe', 'd2glide_sim_test.exe') {
	if (-not (Test-Path (Join-Path $bin $f))) { $missing += $f }
}
if (-not (Test-Path (Join-Path $DataDir 'DIABDAT.MPQ'))) { $missing += 'DIABDAT.MPQ' }
if (-not $python) { $missing += 'python' }
if ($missing.Count -gt 0) { Write-Step "missing: $($missing -join ', ')"; Stop-Watchdog; exit $Script:ExitMissingPrereq }
if (-not $D2CommonDll -or -not (Test-Path $D2CommonDll)) { Write-Step "D2Common.dll not found: parity reference tests will be skipped (and fail)" }
if ($BenchRuns -gt 0 -and (-not $StockExe -or -not (Test-Path $StockExe))) { Write-Step "stock exe not found: $StockExe"; Stop-Watchdog; exit $Script:ExitMissingPrereq }

# The tests look for the game data next to themselves: hard link (no copy, no change to the original)
foreach ($mpq in 'DIABDAT.MPQ', 'devilutionx.mpq') {
	$mpqLink = Join-Path $bin $mpq
	if (-not (Test-Path $mpqLink) -and (Test-Path (Join-Path $DataDir $mpq))) { New-Item -ItemType HardLink -Path $mpqLink -Target (Join-Path $DataDir $mpq) | Out-Null }
}

$run = Join-Path $PSScriptRoot ("runs\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $run | Out-Null
$fixture = Join-Path $Script:RepoDir 'test\fixtures\timedemo\WarriorLevel1to2'
$steps = New-Object System.Collections.ArrayList
$errors = @{}
$bench = @{ stock = @(); mod_d1 = @(); mod_d2 = @() }

function Add-StepResult($Name, $Result) {
	[void]$steps.Add(@{ name = $Name; exitCode = $Result.ExitCode; seconds = $Result.Seconds })
	$state = if ($Result.TimedOut) { 'TIMEOUT' } elseif ($Result.ExitCode -eq 0) { 'ok' } else { "exit $($Result.ExitCode)" }
	Write-Step ("{0,-14} {1} ({2}s)" -f $Name, $state, $Result.Seconds)
}

# Error lines in a process's output. "fread failed" is stock demo playback reading to the end of the
# .dmo file (every replay prints it, stock builds included), so it is not counted.
function Find-Errors($Text) {
	$found = @(($Text -split "`n") | Where-Object { $_ -match '(?i)\b(error|assert|fatal|exception|crash)\b' -and $_ -notmatch '(?i)fread failed|0 errors|\[\s+(OK|PASSED)' } | ForEach-Object { $_.Trim() } | Select-Object -First 10)
	return , $found
}

# 1. Headless scenario tests (parity, collision, stock mode, probes)
$env1 = @{ D2_HARNESS_SAVE_DIR = $run; D2_COMMON_DLL = [string]$D2CommonDll; D2PROBE_LOG = (Join-Path $run 'probe_harness.log') }
$r = Invoke-Bounded -FilePath (Join-Path $bin 'd2harness_test.exe') -ArgumentList @("--gtest_output=xml:$(Join-Path $run 'd2harness.xml')") -TimeoutSeconds 300 -LogFile (Join-Path $run 'd2harness.log') -WorkingDirectory $bin -Environment $env1
Add-StepResult 'harness' $r
$r = Invoke-Bounded -FilePath (Join-Path $bin 'd2mod_test.exe') -ArgumentList @("--gtest_output=xml:$(Join-Path $run 'd2mod.xml')") -TimeoutSeconds 120 -LogFile (Join-Path $run 'd2mod.log') -WorkingDirectory $bin
Add-StepResult 'mod unit' $r
$r = Invoke-Bounded -FilePath (Join-Path $bin 'd2glide_sim_test.exe') -ArgumentList @("--gtest_output=xml:$(Join-Path $run 'd2glide.xml')") -TimeoutSeconds 300 -LogFile (Join-Path $run 'd2glide.log') -WorkingDirectory $bin
Add-StepResult 'glide sim' $r

# 2. Full game replay in both movement modes, probes on
foreach ($mode in 'd1', 'd2') {
	$demoDir = Join-Path $run "demo_$mode"
	Copy-Item -Recurse $fixture $demoDir
	$envDemo = @{ D2_DEMO_DIR = $demoDir; D2_DEMO_MODE = $mode; D2PROBE_LOG = (Join-Path $run "probe_$mode.log") }
	$r = Invoke-Bounded -FilePath (Join-Path $bin 'd2demo_test.exe') -ArgumentList @("--gtest_output=xml:$(Join-Path $run "demo_$mode.xml")") -TimeoutSeconds 600 -LogFile (Join-Path $run "demo_$mode.log") -WorkingDirectory $bin -Environment $envDemo
	Add-StepResult "demo $mode" $r
	$errors["demo $mode"] = Find-Errors $r.Output
}

# 3. FPS benchmark: the same demo with rendering and audio, frame limiting off (timedemo).
# The recording is a shareware (spawn) game, so each build runs from a small install folder where
# spawn.mpq is a hard link to DIABDAT.MPQ (what the unit tests do in memory). Shareware music has
# its own file names that DIABDAT.MPQ lacks, so those three tracks are silent WAVs; every other
# sound plays normally. Identical folders for every build keep the comparison fair.
function New-SilentWav($Path) {
	$rate = 22050; $samples = $rate
	$ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter($ms)
	$w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + 2 * $samples)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
	$w.Write([int]16); $w.Write([int16]1); $w.Write([int16]1); $w.Write([int]$rate); $w.Write([int](2 * $rate)); $w.Write([int16]2); $w.Write([int16]16)
	$w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int](2 * $samples)); $w.Write((New-Object byte[] (2 * $samples)))
	[IO.File]::WriteAllBytes($Path, $ms.ToArray())
}
function New-BenchInstall($Name, $Exe) {
	$dir = Join-Path $run "install_$Name"
	New-Item -ItemType Directory -Force (Join-Path $dir 'assets\music') | Out-Null
	Copy-Item $Exe (Join-Path $dir 'devilutionx.exe') -Force
	New-Item -ItemType HardLink -Path (Join-Path $dir 'spawn.mpq') -Target (Join-Path $DataDir 'DIABDAT.MPQ') | Out-Null
	New-Item -ItemType HardLink -Path (Join-Path $dir 'devilutionx.mpq') -Target (Join-Path $DataDir 'devilutionx.mpq') | Out-Null
	foreach ($track in 'sintro', 'stowne', 'slvla') { New-SilentWav (Join-Path $dir "assets\music\$track.wav") }
	return (Join-Path $dir 'devilutionx.exe')
}
function Invoke-Timedemo($Name, $Exe, $Mode) {
	$dir = Join-Path $run "bench_$Name"
	if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
	Copy-Item -Recurse $fixture $dir
	if ($Mode) { Set-Content -Path (Join-Path $dir 'diablo.ini') -Value "[Game]`r`nDiablo 2 Movement=$Mode" -Encoding ASCII }
	# Probe logging stays off here: the FPS run must cost exactly what a player's game costs
	$demoArgs = @('--save-dir', $dir, '--config-dir', $dir, '--spawn', '--diablo', '--demo', '0', '--timedemo')
	$r = Invoke-Bounded -FilePath $Exe -ArgumentList $demoArgs -TimeoutSeconds 300 -LogFile (Join-Path $dir 'output.log') -WorkingDirectory (Split-Path $Exe)
	$fps = $null
	$m = [regex]::Match($r.Output, '(\d+) frames, ([\d.]+) seconds: ([\d.]+) fps')
	if ($m.Success) { $fps = [double]$m.Groups[3].Value }
	Add-StepResult "bench $Name" $r
	$errors["bench $Name"] = Find-Errors $r.Output
	return $fps
}
if ($BenchRuns -gt 0) {
	$stockInstall = New-BenchInstall 'stock' $StockExe
	$modInstall = New-BenchInstall 'mod' $modExe
}
for ($i = 1; $i -le $BenchRuns; $i++) {
	foreach ($b in @(@('stock', $stockInstall, $null), @('mod_d1', $modInstall, '0'), @('mod_d2', $modInstall, '1'))) {
		$fps = Invoke-Timedemo "$($b[0])_$i" $b[1] $b[2]
		if ($fps) { $bench[$b[0]] += $fps; Write-Step ("  {0}: {1:N1} fps" -f $b[0], $fps) }
	}
}

@{ steps = $steps; bench = $bench; errors = $errors } | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $run 'steps.json') -Encoding UTF8

# 4. Filter into pass/fail
$r = Invoke-Bounded -FilePath $python -ArgumentList @((Join-Path $PSScriptRoot 'filter_probes.py'), $run) -TimeoutSeconds 150 -LogFile (Join-Path $run 'filter.log')
Stop-Watchdog
Write-Host $r.Output
Write-Step "results: $run"
switch ($r.ExitCode) {
	0 { exit $Script:ExitPass }
	1 { exit $Script:ExitFail }
	3 { exit $Script:ExitMissingPrereq }
	124 { exit $Script:ExitTimeout }
	default { exit $Script:ExitCrash }
}
