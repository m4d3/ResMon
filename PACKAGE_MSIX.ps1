param(
    [string]$IdentityName = "ResMon.Local",
    [string]$Publisher = "CN=ResMon Local",
    [string]$Version = "1.7.2.0"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$exe = Join-Path $PSScriptRoot "build\Release\ResMon.exe"
if (-not (Test-Path $exe)) {
    $exe = Join-Path $PSScriptRoot "build\ResMon.exe"
}
if (-not (Test-Path $exe)) {
    throw "Build the app first with BUILD_AND_RUN.cmd."
}

$makeAppx = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter MakeAppx.exe -Recurse -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $makeAppx) {
    throw "MakeAppx.exe was not found. Install the Windows 11 SDK."
}

$stage = Join-Path $PSScriptRoot "package\stage"
$out = Join-Path $PSScriptRoot "package\ResMon.msix"
Remove-Item (Join-Path $PSScriptRoot "package") -Recurse -Force -ErrorAction SilentlyContinue
New-Item $stage -ItemType Directory -Force | Out-Null
New-Item (Join-Path $stage "Assets") -ItemType Directory -Force | Out-Null
Copy-Item $exe (Join-Path $stage "ResMon.exe")
Copy-Item (Join-Path $PSScriptRoot "assets\Square44x44Logo.png") (Join-Path $stage "Assets")
Copy-Item (Join-Path $PSScriptRoot "assets\Square150x150Logo.png") (Join-Path $stage "Assets")
Copy-Item (Join-Path $PSScriptRoot "assets\StoreLogo.png") (Join-Path $stage "Assets")
Copy-Item (Join-Path $PSScriptRoot "assets\Square310x310Logo.png") (Join-Path $stage "Assets")
Copy-Item (Join-Path $PSScriptRoot "assets\Wide310x150Logo.png") (Join-Path $stage "Assets")

$manifest = Get-Content (Join-Path $PSScriptRoot "packaging\AppxManifest.template.xml") -Raw
$manifest = $manifest.Replace("__IDENTITY_NAME__", $IdentityName).Replace("__PUBLISHER__", $Publisher).Replace("__VERSION__", $Version)
Set-Content (Join-Path $stage "AppxManifest.xml") $manifest -Encoding UTF8

& $makeAppx.FullName pack /d $stage /p $out /o
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed." }

Write-Host ""
Write-Host "Created: $out"
Write-Host ""
Write-Host "The package is intentionally unsigned. For Microsoft Store submission, replace IdentityName/Publisher"
Write-Host "with the exact values assigned by Partner Center, then sign or upload according to Store requirements."
