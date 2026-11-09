# Builds the release: the mod, the VR host and the launcher, then KotorVR-Setup.exe (PyInstaller, one file) and
# dist\KotorVR-<version>.zip (the setup + the guides).
#   .\installer\build_installer.ps1
# Nothing of BioWare's goes in: the setup builds the VR assets from the player's own game files.
$ErrorActionPreference = 'Continue'  # native tools write warnings to stderr; exit codes are checked
$root = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path "$PSScriptRoot\kotorvr_setup.py" -Pattern '^VERSION = "(.+)"').Matches[0].Groups[1].Value

foreach ($b in 'game\build.bat', 'host\build.bat', 'host\launch_build.bat') {
    & cmd.exe /c "$root\$b" 2>$null | Select-String -Pattern 'error|built' | ForEach-Object { $_.Line }
    if ($LASTEXITCODE -ne 0) { throw "build failed: $b" }
}

$stage = Join-Path $root 'installer\build\payload'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\tools", "$stage\docs" | Out-Null
Copy-Item "$root\game\build\dinput8.dll" $stage
Copy-Item "$root\host\build\kvr_host.exe", "$root\host\build\openxr_loader.dll", "$root\host\build\kvr_launch.exe" $stage
foreach ($t in 'kres', 'mdl', 'weapon_export', 'hand_export', 'icon_export', 'sound_export', 'action_names', 'vis_override', 'door_export', 'sig_export') {
    Copy-Item "$root\tools\$t.py" "$stage\tools"
}
Copy-Item "$root\README.md", "$root\LICENSE" "$stage\docs"
Copy-Item "$root\docs\USER_GUIDE.md", "$root\docs\KNOWN_ISSUES.md", "$root\CHANGELOG.md" "$stage\docs"

$work = Join-Path $root 'installer\build\pyi'
python -m PyInstaller --noconfirm --clean --onefile --console --name KotorVR-Setup `
    --distpath "$root\installer\build\dist" --workpath $work --specpath $work `
    --add-data "$stage;payload" --hidden-import PIL --hidden-import PIL.Image --hidden-import configparser `
    "$PSScriptRoot\kotorvr_setup.py" 2>&1 | Select-String -Pattern 'ERROR|completed successfully' | ForEach-Object { $_.Line }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$zipDir = Join-Path $root "installer\build\KotorVR-$version"
if (Test-Path $zipDir) { Remove-Item $zipDir -Recurse -Force }
New-Item -ItemType Directory -Force $zipDir | Out-Null
Copy-Item "$root\installer\build\dist\KotorVR-Setup.exe" $zipDir
Copy-Item "$root\README.md", "$root\LICENSE", "$root\CHANGELOG.md", "$root\docs\USER_GUIDE.md", "$root\docs\KNOWN_ISSUES.md" $zipDir
$zip = Join-Path $dist "KotorVR-$version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$zipDir\*" -DestinationPath $zip
Write-Host "release: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
