# Headset-free test run: simulated host + KotOR, scripted, artifacts collected.
#
#   .\tools\run_sim.ps1 -Script spin            # tools\sim\spin.txt
#   .\tools\run_sim.ps1 -Script walk -KeepGame  # leave KotOR running afterwards
#   .\tools\run_sim.ps1 -Script spin -Force     # stop a running real kvr_host first
#
# Artifacts: traces\sim\<script>_<timestamp>\  (PNGs, stats.csv, simhost.log, probe.log)
# Note: menu clicks move the real mouse cursor; leave the PC alone while a run is going.
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [int]$TimeoutSec = 300,
    [switch]$KeepGame,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$game = 'C:\Program Files (x86)\Steam\steamapps\common\swkotor'
$scriptPath = if (Test-Path $Script) { (Resolve-Path $Script).Path } else { Join-Path $root "tools\sim\$Script.txt" }
if (-not (Test-Path $scriptPath)) { throw "script not found: $scriptPath" }
$name = [IO.Path]::GetFileNameWithoutExtension($scriptPath)
$out = Join-Path $root ("traces\sim\{0}_{1}" -f $name, (Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Force $out | Out-Null
$simhost = Join-Path $root 'host\build\kvr_simhost.exe'
if (-not (Test-Path $simhost)) { throw "build the simulator first: host\simhost_build.bat" }

# --- preflight -------------------------------------------------------------------------------
if (Get-Process Discord -ErrorAction SilentlyContinue) {
    throw "Discord is running - its overlay leaks a GL context per frame and kills the game. Quit it first."
}
# a real host would fight the simulator over shared memory (a 1-thread zombie from a lost runtime is harmless)
$realHosts = Get-Process kvr_host -ErrorAction SilentlyContinue | Where-Object { $_.Threads.Count -gt 1 }
if ($realHosts) {
    if (-not $Force) { throw "a real kvr_host is running (PID $($realHosts.Id -join ', ')). Stop it or pass -Force." }
    $realHosts | Stop-Process -Force; Start-Sleep 2
}
Get-Process kvr_simhost -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process swkotor -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
Remove-Item "$game\kvr\probe.log" -ErrorAction SilentlyContinue
# KotOR rewrites swkotor.ini on exit; a force-kill mid-write mangled it once. Snapshot it and put it back afterwards.
$iniSnapshot = Join-Path $out 'swkotor.ini.before'
Copy-Item "$game\swkotor.ini" $iniSnapshot
# Warps into new modules make KotOR autosave over the player's AUTOSAVE slot: snapshot the saves and put them back.
$savesSnapshot = Join-Path $out 'saves.before'
Copy-Item "$game\saves" $savesSnapshot -Recurse

# --- run ---------------------------------------------------------------------------------------
$log = Join-Path $out 'simhost.log'
$sim = Start-Process -FilePath $simhost -ArgumentList "`"$scriptPath`"", "`"$out`"" -PassThru -NoNewWindow `
    -RedirectStandardOutput $log -RedirectStandardError (Join-Path $out 'simhost.err')
$null = $sim.Handle  # keep a handle so ExitCode is available after exit
Start-Sleep 1
& 'C:\Program Files (x86)\Steam\steam.exe' -applaunch 32370
Write-Host "running '$name' (timeout $TimeoutSec s) -> $out"
if (-not $sim.WaitForExit($TimeoutSec * 1000)) {
    Write-Host "TIMEOUT - stopping the simulator"
    $sim | Stop-Process -Force
}
$code = if ($sim.HasExited) { $sim.ExitCode } else { -1 }

# --- collect -----------------------------------------------------------------------------------
if (-not $KeepGame) {
    Get-Process swkotor -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1
    Copy-Item $iniSnapshot "$game\swkotor.ini" -Force
    # only replace the live saves with a complete snapshot (same save folders as when the run started)
    $snapCount = @(Get-ChildItem $savesSnapshot -Directory -ErrorAction SilentlyContinue).Count
    if ((Test-Path $savesSnapshot) -and $snapCount -gt 0) {
        Remove-Item "$game\saves" -Recurse -Force
        Copy-Item $savesSnapshot "$game\saves" -Recurse
        Remove-Item $savesSnapshot -Recurse -Force
    } else {
        Write-Host "WARNING: saves snapshot missing or empty; live saves left as they are"
    }
}
if (Test-Path "$game\kvr\probe.log") { Copy-Item "$game\kvr\probe.log" (Join-Path $out 'probe.log') }
$probe = Join-Path $out 'probe.log'
$stalls = if (Test-Path $probe) { @(Select-String -Path $probe -Pattern 'STALL') } else { @() }
$shots = @(Get-ChildItem $out -Filter *.png)

Write-Host ""
Write-Host "=== $name : $(if ($code -eq 0) { 'PASSED' } else { "FAILED (exit $code)" }) ==="
Get-Content $log | Select-String -Pattern 'TIMED OUT|FAILED|unknown|frames pulled|opened shared' | ForEach-Object { "  $_" }
Write-Host "  screenshots: $($shots.Count)"
Write-Host "  stalls in probe.log: $($stalls.Count)"
$stalls | Select-Object -First 5 | ForEach-Object { "    $($_.Line)" }
Write-Host "  artifacts: $out"
exit $code
