# Packs a build of this repository into package/stellaris-live2d-<version>.zip (plus its SHA-256 file). Used by the CI workflow
# (.github/workflows/build-release.yml); run it by hand after a Release build to try the package:
#
#     cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ; cmake --build build --config Release
#     pwsh scripts/package_release.ps1 -Version dev-local
#
# The zip holds the plugin and the loader (python scripts\deploy.py installs both from the unpacked folder), the helper scripts, the
# command line tools (l2d_pack, l2d_view, l2d_check when it has been built), the docs and a GAME_BUILD.txt that names the game build.
param(
    [string]$Version = "dev-local",
    [string]$Build = "build",
    [string]$Out = "package"
)
$ErrorActionPreference = "Stop"

# the DLL only works with the stellaris.exe build the SDK was generated from
$sdk = "sdk/stellaris_sdk.hpp"
$ts = (Select-String -Path $sdk -Pattern "kExeTimestamp = (0x[0-9A-Fa-f]+)").Matches[0].Groups[1].Value
$built = [DateTimeOffset]::FromUnixTimeSeconds([Convert]::ToInt64($ts, 16)).UtcDateTime.ToString("yyyy-MM-dd HH:mm 'UTC'")

$name = "stellaris-live2d-$Version"
$dir = Join-Path $Out $name
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Force -Path "$dir/scripts", "$dir/tools", "$dir/docs" | Out-Null

function Need($path) {
    if (-not (Test-Path $path)) { throw "missing build output: $path" }
    return $path
}
Copy-Item (Need "$Build/Release/stellaris_live2d.dll") $dir
Copy-Item (Need "$Build/loader/d3dx9_43.dll") $dir
foreach ($s in "deploy.py", "l2dctl.py", "desktop_guard.py") { Copy-Item (Need "scripts/$s") "$dir/scripts/" }
foreach ($t in "l2d_pack", "l2d_view", "l2d_check") {
    $exe = "$Build/Release/$t.exe"
    if (Test-Path $exe) { Copy-Item $exe "$dir/tools/" }
    elseif ($t -ne "l2d_check") { throw "missing build output: $exe" }
}
Copy-Item (Need "tools/motion_diff.py") "$dir/tools/"
Copy-Item (Need "docs/portrait-mod-design.md") "$dir/docs/"
Copy-Item README.md, README.zh-CN.md, LICENSE $dir

@(
    "Built for the stellaris.exe whose PE timestamp is $ts ($built), Stellaris 4.5.1 (Windows x64).",
    "With any other build the DLL logs the mismatch and installs nothing.",
    "",
    "Install: python scripts\deploy.py   (copies stellaris_live2d.dll and d3dx9_43.dll next to stellaris.exe; --remove undoes it)",
    "The plugin needs a Cubism Core library (Live2DCubismCore.dll) that is not included: see README.md."
) | Set-Content -Encoding utf8 "$dir/GAME_BUILD.txt"

Get-ChildItem -Recurse -Directory -Filter __pycache__ $dir | Remove-Item -Recurse -Force
$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $dir -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $name.zip" | Set-Content -Encoding ascii "$zip.sha256"

Write-Host "packed $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 2)) MB), game build $ts ($built)"
if ($env:GITHUB_OUTPUT) {
    "version=$Version" >> $env:GITHUB_OUTPUT
    "exe_timestamp=$ts" >> $env:GITHUB_OUTPUT
    "exe_built=$built" >> $env:GITHUB_OUTPUT
}
