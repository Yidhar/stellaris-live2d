# Packs a build of this repository into package/stellaris-live2d-<version>.zip (plus its SHA-256 file). Used by the CI workflow
# (.github/workflows/build-release.yml); run it by hand after a Release build to try the package:
#
#     cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ; cmake --build build --config Release
#     pwsh scripts/package_release.ps1 -Version dev-local
#
# The zip holds the plugin, the loader and a Cubism Core (Purism Core, built by CMake from third_party/purism_core): python scripts\deploy.py
# installs all three from the unpacked folder and writes the ini, so the plugin works as soon as a portrait mod is enabled. It also holds the
# helper scripts, the command line tools (l2d_pack, l2d_view, l2d_check when it has been built), the docs, the licenses of what is built into
# the DLLs (THIRD_PARTY_NOTICES.txt) and a GAME_BUILD.txt that names the game build.
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
Copy-Item (Need "$Build/Release/Live2DCubismCore.dll") $dir
foreach ($s in "deploy.py", "l2dctl.py", "desktop_guard.py") { Copy-Item (Need "scripts/$s") "$dir/scripts/" }
foreach ($t in "l2d_pack", "l2d_view", "l2d_check") {
    $exe = "$Build/Release/$t.exe"
    if (Test-Path $exe) { Copy-Item $exe "$dir/tools/" }
    elseif ($t -ne "l2d_check") { throw "missing build output: $exe" }
}
Copy-Item (Need "tools/motion_diff.py") "$dir/tools/"
Copy-Item (Need "docs/portrait-mod-design.md") "$dir/docs/"
Copy-Item README.md, README.zh-CN.md, LICENSE $dir

# the licenses that ask to travel with binaries: Purism Core (built unchanged into Live2DCubismCore.dll), MinHook and nlohmann/json (built into
# the plugin). miniaudio and the stb libraries are public domain / MIT-0 and need no notice.
$notices = @(
    "Third-party software in this package",
    "====================================",
    "",
    "Live2DCubismCore.dll is Purism Core 1.1.0 (https://github.com/SakuraMotion/PurismCore), built unchanged from its single-file release.",
    "It is a compatible reimplementation of the Cubism Core API, not Live2D's own library; any official Live2DCubismCore.dll can be used in",
    "its place (core_dll in stellaris_live2d.ini).",
    "",
    "-- Purism Core license --",
    (Get-Content -Raw (Need "third_party/purism_core/LICENSE")),
    "",
    "-- MinHook (built into stellaris_live2d.dll), https://github.com/TsudaKageyu/minhook --",
    (Get-Content -Raw (Need "$Build/_deps/minhook-src/LICENSE.txt")),
    "",
    "-- nlohmann/json 3.11 (built into stellaris_live2d.dll), https://github.com/nlohmann/json --",
    "MIT License",
    "",
    "Copyright (c) 2013-2023 Niels Lohmann",
    "",
    "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the",
    '"Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish,',
    "distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to",
    "the following conditions:",
    "",
    "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.",
    "",
    'THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF',
    "MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR",
    "ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE",
    "SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE."
)
$notices | Set-Content -Encoding utf8 "$dir/THIRD_PARTY_NOTICES.txt"

@(
    "Built for the stellaris.exe whose PE timestamp is $ts ($built), Stellaris 4.5.1 (Windows x64).",
    "With any other build the DLL logs the mismatch and installs nothing.",
    "",
    "Install: python scripts\deploy.py   (copies stellaris_live2d.dll, d3dx9_43.dll and Live2DCubismCore.dll next to stellaris.exe and writes",
    "stellaris_live2d.ini; --remove undoes it). Then start the game with a portrait mod enabled: see the demo mod, README.md.",
    "Live2DCubismCore.dll is Purism Core (see THIRD_PARTY_NOTICES.txt); to use Live2D's official library, point core_dll in the ini at it."
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
