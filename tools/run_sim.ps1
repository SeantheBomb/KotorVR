# Headset-free test run: simulated host + KotOR, scripted, artifacts collected.
#
#   .\tools\run_sim.ps1 -Script spin            # tools\sim\spin.txt
#   .\tools\run_sim.ps1 -Script walk -KeepGame  # leave KotOR running afterwards
#   .\tools\run_sim.ps1 -Script spin -Force     # stop a running real kvr_host first
#   .\tools\run_sim.ps1 -Script t_door -Fixture door_starboard   # start from fixtures\saves\door_starboard: the only
#                                                 # save in the list during the run (scripts load the top row)
#   .\tools\run_sim.ps1 -Script mk_x -SaveFixture x   # keep the run's newest save (F4 quicksave) as fixture x
#
# Artifacts: traces\sim\<script>_<timestamp>\  (PNGs, stats.csv, simhost.log, probe.log)
# Note: menu clicks move the real mouse cursor; leave the PC alone while a run is going.
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [int]$TimeoutSec = 300,
    [switch]$KeepGame,
    [switch]$Force,
    [switch]$AllowDiscord,  # run with Discord open (compatibility testing)
    [string]$Fixture,       # fixtures\saves\<name>: the one save present during the run
    [string]$SaveFixture,   # after the run, keep its newest save as fixtures\saves\<name>
    [switch]$Cheats         # for this run: cheat console on, no autopause when an enemy is sighted
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
if (-not $AllowDiscord -and (Get-Process Discord -ErrorAction SilentlyContinue)) {
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
# the log of a real play session is evidence: keep it (traces\play\) before a run replaces it
$prev = Get-Item "$game\kvr\probe.log" -ErrorAction SilentlyContinue
$lastSim = Get-ChildItem (Join-Path $root 'traces\sim') -Recurse -Filter probe.log -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($prev -and -not ($lastSim -and $lastSim.Length -eq $prev.Length)) {  # (a sim run's log was already collected)
    $playDir = Join-Path $root 'traces\play'
    New-Item -ItemType Directory -Force $playDir | Out-Null
    Copy-Item $prev.FullName (Join-Path $playDir ("probe_{0}.log" -f $prev.LastWriteTime.ToString('yyyyMMdd_HHmmss')))
}
Remove-Item "$game\kvr\probe.log" -ErrorAction SilentlyContinue
# KotOR rewrites swkotor.ini on exit; a force-kill mid-write mangled it once. Snapshot it and put it back afterwards.
$iniSnapshot = Join-Path $out 'swkotor.ini.before'
Copy-Item "$game\swkotor.ini" $iniSnapshot
if ($Cheats) {
    $ini = Get-Content "$game\swkotor.ini"
    $ini = $ini -replace '^Enemy Sighted=1$', 'Enemy Sighted=0'
    if (-not ($ini -match '^EnableCheats=')) {
        $i = [Array]::IndexOf($ini, '[Game Options]')
        if ($i -ge 0) { $ini = $ini[0..$i] + 'EnableCheats=1' + $ini[($i + 1)..($ini.Count - 1)] }
    } else { $ini = $ini -replace '^EnableCheats=.*$', 'EnableCheats=1' }
    Set-Content "$game\swkotor.ini" $ini -Encoding ascii
}
# Warps into new modules make KotOR autosave over the player's AUTOSAVE slot: snapshot the saves and put them back.
$savesSnapshot = Join-Path $out 'saves.before'
Copy-Item "$game\saves" $savesSnapshot -Recurse
Remove-Item "$game\kvr\simcmd.txt" -ErrorAction SilentlyContinue
if ($Fixture) {
    $fx = Join-Path $root "fixtures\saves\$Fixture"
    if (-not (Test-Path $fx)) { throw "fixture not found: $fx" }
    Get-ChildItem "$game\saves" | Remove-Item -Recurse -Force
    Copy-Item "$fx\*" "$game\saves" -Recurse
    Write-Host "fixture: $Fixture"
}

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
    if ($SaveFixture) {
        $newest = Get-ChildItem "$game\saves" -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $fx = Join-Path $root "fixtures\saves\$SaveFixture"
        if ($newest -and $newest.LastWriteTime -gt (Get-Item $out).CreationTime) {
            if (Test-Path $fx) { Remove-Item $fx -Recurse -Force }
            New-Item -ItemType Directory -Force $fx | Out-Null
            Copy-Item $newest.FullName $fx -Recurse
            Write-Host "fixture saved: $SaveFixture <- $($newest.Name)"
        } else { Write-Host "WARNING: no save written during the run; fixture $SaveFixture not saved" }
    }
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
Get-Content $log | Select-String -Pattern 'TIMED OUT|FAILED|unknown|frames pulled|opened shared|CHECK ' | ForEach-Object { "  $_" }
Write-Host "  screenshots: $($shots.Count)"
Write-Host "  stalls in probe.log: $($stalls.Count)"
$stalls | Select-Object -First 5 | ForEach-Object { "    $($_.Line)" }
Write-Host "  artifacts: $out"
exit $code
