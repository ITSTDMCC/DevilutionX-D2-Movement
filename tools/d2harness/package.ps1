<#
.SYNOPSIS
  Put the modded exe in a ready-to-play folder: double-click devilutionx.exe there and it runs.
  Game data (DIABDAT.MPQ, Hellfire MPQs, devilutionx.mpq) is hard linked from the stock folder:
  no extra disk space, and the originals are never modified.

.PARAMETER BuildDir  Mod build (default build-d2parity).
.PARAMETER OutDir    Folder to create (default ..\devilutionx-d2movement). Refuses to overwrite a different exe there.
.PARAMETER Force     Replace an existing exe in OutDir.

Exit codes: 0 done, 1 refused (OutDir has a different exe, use -Force), 3 missing prerequisite, 124 timeout.
#>
param(
	[string]$BuildDir,
	[string]$OutDir,
	[switch]$Force
)
. (Join-Path $PSScriptRoot 'common.ps1')
Start-Watchdog 5
if (-not $BuildDir) { $BuildDir = Join-Path $Script:RepoDir 'build-d2parity' }
if (-not $OutDir) { $OutDir = Join-Path $Script:Diablo1Dir 'devilutionx-d2movement' }
$stock = Join-Path $Script:Diablo1Dir 'devilutionx'
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
