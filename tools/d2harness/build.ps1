<#
.SYNOPSIS
  Configure and build a DevilutionX tree with Visual Studio 2022, reusing the dependency sources
  already fetched by the existing build/ and build-tests/ folders (nothing is downloaded).

.PARAMETER SourceDir   Tree to build (default: this repository).
.PARAMETER BuildDir    New build folder. Existing build/ and build-tests/ are never touched.
.PARAMETER Testing     Build the unit tests and the D2 harness tests (without LTO, which MSVC builds need for testing).
.PARAMETER Targets     MSBuild targets (default: everything).
.PARAMETER MaxMinutes  Hard limit for the whole script (default 45).

Exit codes: 0 built, 2 configure/compile failed, 3 missing prerequisite, 124 timed out.
#>
param(
	[string]$SourceDir,
	[Parameter(Mandatory)] [string]$BuildDir,
	[switch]$Testing,
	[string[]]$Targets = @(),
	[double]$MaxMinutes = 45
)
. (Join-Path $PSScriptRoot 'common.ps1')
if (-not $SourceDir) { $SourceDir = $Script:RepoDir }
Start-Watchdog $MaxMinutes

if (-not (Test-Path $Script:CMakeExe)) { Write-Step "cmake not found at $($Script:CMakeExe)"; Stop-Watchdog; exit $Script:ExitMissingPrereq }

# Dependency sources from the earlier builds (read-only use via FETCHCONTENT_SOURCE_DIR_*)
$depRoots = @((Join-Path $Script:RepoDir 'build-tests\_deps'), (Join-Path $Script:RepoDir 'build\_deps'))
$depArgs = @('-DFETCHCONTENT_FULLY_DISCONNECTED=ON')
foreach ($dep in 'asio', 'bzip2', 'find_steam_game', 'googletest', 'libfmt', 'libmpq', 'libpng', 'libsmackerdec', 'libsodium', 'sdl2', 'sdl_audiolib', 'sdl_image', 'simpleini', 'zlib') {
	$found = $depRoots | ForEach-Object { Join-Path $_ "$dep-src" } | Where-Object { Test-Path $_ } | Select-Object -First 1
	if (-not $found) {
		if ($dep -eq 'googletest' -and -not $Testing) { continue }
		Write-Step "dependency source $dep-src not found in build/ or build-tests/"
		Stop-Watchdog; exit $Script:ExitMissingPrereq
	}
	$depArgs += "-DFETCHCONTENT_SOURCE_DIR_$($dep.ToUpper())=$($found -replace '\\', '/')"
}

New-Item -ItemType Directory -Force $BuildDir | Out-Null
$configureArgs = @('-S', $SourceDir, '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64',
	'-DCMAKE_POLICY_VERSION_MINIMUM=3.5', ('-DBUILD_TESTING=' + $(if ($Testing) { 'ON' } else { 'OFF' })),
	# MSVC builds switch testing off when LTO is on, so test builds go without LTO (release exes keep it)
	('-DDISABLE_LTO=' + $(if ($Testing) { 'ON' } else { 'OFF' })),
	'-DDISABLE_ZERO_TIER=ON', '-DDISCORD_INTEGRATION=OFF',
	'-DDEVILUTIONX_SYSTEM_BZIP2=OFF', '-DDEVILUTIONX_SYSTEM_LIBFMT=OFF', '-DDEVILUTIONX_SYSTEM_LIBSODIUM=OFF',
	'-DDEVILUTIONX_SYSTEM_SDL2=OFF', '-DDEVILUTIONX_SYSTEM_SDL_AUDIOLIB=OFF', '-DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF',
	'-DDEVILUTIONX_SYSTEM_SIMPLEINI=OFF', '-DDEVILUTIONX_SYSTEM_ZLIB=OFF', '-DDEVILUTIONX_SYSTEM_GOOGLETEST=OFF',
	'-DDEVILUTIONX_SYSTEM_LIBPNG=OFF') + $depArgs

Write-Step "configure $BuildDir"
$r = Invoke-Bounded -FilePath $Script:CMakeExe -ArgumentList $configureArgs -TimeoutSeconds 900 -LogFile (Join-Path $BuildDir 'configure.log')
if ($r.TimedOut) { Write-Step 'configure timed out'; Stop-Watchdog; exit $Script:ExitTimeout }
if ($r.ExitCode -ne 0) { Write-Step "configure failed ($($r.ExitCode)), see configure.log"; Stop-Watchdog; exit $Script:ExitBuildError }

$buildArgs = @('--build', $BuildDir, '--config', 'Release', '--parallel')
if ($Targets.Count -gt 0) { $buildArgs += @('--target') + $Targets }
Write-Step "build $BuildDir ($($r.Seconds)s configure)"
$r = Invoke-Bounded -FilePath $Script:CMakeExe -ArgumentList $buildArgs -TimeoutSeconds 3600 -LogFile (Join-Path $BuildDir 'build.log')
Stop-Watchdog
if ($r.TimedOut) { Write-Step 'build timed out'; exit $Script:ExitTimeout }
if ($r.ExitCode -ne 0) {
	Write-Step "build failed ($($r.ExitCode)), errors:"
	($r.Output -split "`n") | Where-Object { $_ -match ' error ' } | Select-Object -First 20 | ForEach-Object { Write-Host $_ }
	exit $Script:ExitBuildError
}
Write-Step "built in $($r.Seconds)s"
exit $Script:ExitPass
