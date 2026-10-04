# Part of DevilutionX-D2-Movement. Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
# Licensed under the Sustainable Use License (LICENSE.md); see NOTICE-D2MOVEMENT.md.
<#
.SYNOPSIS
  Put the modded exe in a ready-to-play folder: double-click devilutionx.exe there and it runs.
  Your own game data (DIABDAT.MPQ, Hellfire MPQs, devilutionx.mpq) is hard linked from DataDir:
  no extra disk space, and the originals are never modified. OutDir must be outside this repository,
  so the game data linked there can never be committed.

.PARAMETER BuildDir  Mod build (default build-d2parity).
.PARAMETER DataDir   Folder with your MPQs (setting DataDir). Required.
.PARAMETER OutDir    Folder to create (setting OutDir). Required. Refuses to overwrite a different exe there.
.PARAMETER Force     Replace an existing exe in OutDir.

Exit codes: 0 done, 1 refused (OutDir has a different exe, use -Force), 3 missing prerequisite, 124 timeout.
#>
param(
	[string]$BuildDir,
	[string]$DataDir,
	[string]$OutDir,
	[switch]$Force
)
. (Join-Path $PSScriptRoot 'common.ps1')
Start-Watchdog 5
if (-not $BuildDir) { $BuildDir = Join-Path $Script:RepoDir 'build-d2parity' }
$OutDir = Get-Setting 'OutDir' $OutDir
$stock = Get-Setting 'DataDir' $DataDir
if (-not $OutDir -or -not $stock) { Write-Step 'OutDir and DataDir must be set (parameters, D2H_* variables or local.psd1)'; Stop-Watchdog; exit $Script:ExitMissingPrereq }
$fullOut = [IO.Path]::GetFullPath($OutDir)
if ($fullOut.StartsWith($Script:RepoDir, [StringComparison]::OrdinalIgnoreCase)) {
	Write-Step "OutDir is inside the repository; pick a folder outside it so game data cannot be committed"
	Stop-Watchdog; exit $Script:ExitFail
}
$exe = Join-Path $BuildDir 'Release\devilutionx.exe'
if (-not (Test-Path $exe)) { Write-Step "no exe at $exe"; Stop-Watchdog; exit $Script:ExitMissingPrereq }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$target = Join-Path $OutDir 'devilutionx.exe'
if ((Test-Path $target) -and -not $Force -and ((Get-FileHash $target).Hash -ne (Get-FileHash $exe).Hash)) {
	Write-Step "$target exists and differs; rerun with -Force to replace it"
	Stop-Watchdog; exit $Script:ExitFail
}
Copy-Item $exe $target -Force
foreach ($f in Get-ChildItem $stock -Filter *.mpq) {
	$link = Join-Path $OutDir $f.Name
	if (-not (Test-Path $link)) { New-Item -ItemType HardLink -Path $link -Target $f.FullName | Out-Null }
}
Copy-Item (Join-Path $Script:HarnessDir 'PLAYING.txt') (Join-Path $OutDir 'README-D2MOVEMENT.txt') -Force
Write-Step "ready: $target"
Stop-Watchdog
exit $Script:ExitPass
