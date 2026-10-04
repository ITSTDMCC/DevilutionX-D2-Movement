# Shared helpers for the D2 movement harness scripts (dot-source this file).
#
# Exit codes used by every harness script (see README.md):
#   0   PASS            everything ran and every check passed
#   1   FAIL            ran to completion but at least one check failed
#   2   BUILD_ERROR     configure or compile failed
#   3   MISSING_PREREQ  a tool, file or MPQ the script needs is not there
#   4   CRASH           a child process died unexpectedly (non-zero exit, no result file)
#   124 TIMEOUT         a step or the whole script hit its hard time limit and was killed

$ErrorActionPreference = 'Continue'

$Script:ExitPass = 0
$Script:ExitFail = 1
$Script:ExitBuildError = 2
$Script:ExitMissingPrereq = 3
$Script:ExitCrash = 4
$Script:ExitTimeout = 124

$Script:HarnessDir = $PSScriptRoot
$Script:RepoDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Script:Diablo1Dir = (Resolve-Path (Join-Path $Script:RepoDir '..')).Path
$Script:CMakeExe = 'C:\Program Files\CMake\bin\cmake.exe'

function Write-Step([string]$Message) {
	Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $Message)
}

# Starts the whole-script watchdog: if the script is still running after $Minutes it kills
# itself and every child process, exiting with 124. Nothing in the harness can run forever.
function Start-Watchdog([double]$Minutes) {
	$Script:WatchdogDeadline = (Get-Date).AddMinutes($Minutes)
	$selfPid = $PID
	$Script:Watchdog = Start-Job -ScriptBlock {
		param($ParentPid, $Seconds)
		Start-Sleep -Seconds $Seconds
		& taskkill.exe /PID $ParentPid /T /F | Out-Null
	} -ArgumentList $selfPid, ([int]($Minutes * 60))
}

function Stop-Watchdog {
	if ($Script:Watchdog) {
		Stop-Job $Script:Watchdog -ErrorAction SilentlyContinue | Out-Null
		Remove-Job $Script:Watchdog -Force -ErrorAction SilentlyContinue | Out-Null
		$Script:Watchdog = $null
	}
}

# Seconds left before the script-wide watchdog fires (a step never gets more than that).
function Get-RemainingSeconds {
	if (-not $Script:WatchdogDeadline) { return [int]::MaxValue }
	return [int][Math]::Max(1, ($Script:WatchdogDeadline - (Get-Date)).TotalSeconds - 5)
}

# Runs a process with a hard time limit. Returns @{ ExitCode; TimedOut; Seconds }.
# On timeout the whole process tree is killed and ExitCode is 124.
function Invoke-Bounded {
	param(
		[Parameter(Mandatory)] [string]$FilePath,
		[string[]]$ArgumentList = @(),
		[Parameter(Mandatory)] [int]$TimeoutSeconds,
		[string]$LogFile,
		[string]$WorkingDirectory = (Get-Location).Path,
		[hashtable]$Environment = @{}
	)
	$limit = [Math]::Min($TimeoutSeconds, (Get-RemainingSeconds))
	$psi = New-Object System.Diagnostics.ProcessStartInfo
	$psi.FileName = $FilePath
	$psi.Arguments = ($ArgumentList | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
	$psi.WorkingDirectory = $WorkingDirectory
	$psi.UseShellExecute = $false
	$psi.CreateNoWindow = $true
	$psi.RedirectStandardOutput = $true
	$psi.RedirectStandardError = $true
	foreach ($k in $Environment.Keys) { $psi.EnvironmentVariables[$k] = [string]$Environment[$k] }

	$proc = New-Object System.Diagnostics.Process
	$proc.StartInfo = $psi
	$out = New-Object System.Text.StringBuilder
	$handler = { if ($EventArgs.Data -ne $null) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } }
	$e1 = Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action $handler -MessageData $out
	$e2 = Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action $handler -MessageData $out
	$sw = [Diagnostics.Stopwatch]::StartNew()
	[void]$proc.Start()
	$proc.BeginOutputReadLine()
	$proc.BeginErrorReadLine()
	$timedOut = -not $proc.WaitForExit($limit * 1000)
	if ($timedOut) {
		& taskkill.exe /PID $proc.Id /T /F 2>&1 | Out-Null
		[void]$proc.WaitForExit(10000)
	} else {
		$proc.WaitForExit()
	}
	$sw.Stop()
	Start-Sleep -Milliseconds 200
	Unregister-Event -SourceIdentifier $e1.Name
	Unregister-Event -SourceIdentifier $e2.Name
	if ($LogFile) { [IO.File]::WriteAllText($LogFile, $out.ToString()) }
	$code = if ($timedOut) { $Script:ExitTimeout } else { $proc.ExitCode }
	return @{ ExitCode = $code; TimedOut = $timedOut; Seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 1); Output = $out.ToString() }
}
