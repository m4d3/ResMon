$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vendor = Join-Path $root "vendor"
New-Item -ItemType Directory -Force -Path $vendor | Out-Null

$pawnIoVersion = "2.2.0"
$lhmVersion = "v0.9.6"
$installer = Join-Path $vendor "PawnIO_setup.exe"
$intel = Join-Path $vendor "IntelMSR.bin"
$amd = Join-Path $vendor "AMDFamily17.bin"

$pawnUrl = "https://github.com/namazso/PawnIO.Setup/releases/download/$pawnIoVersion/PawnIO_setup.exe"
$intelUrl = "https://raw.githubusercontent.com/LibreHardwareMonitor/LibreHardwareMonitor/$lhmVersion/LibreHardwareMonitorLib/Resources/PawnIo/IntelMSR.bin"
$amdUrl = "https://raw.githubusercontent.com/LibreHardwareMonitor/LibreHardwareMonitor/$lhmVersion/LibreHardwareMonitorLib/Resources/PawnIo/AMDFamily17.bin"
$expectedPawnSha256 = "1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032"

function Get-File($url, $path, $name) {
    if (Test-Path $path) {
        Write-Host "  $name already present."
        return
    }
    Write-Host "  Downloading $name..."
    Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
    if ((Get-Item $path).Length -lt 64) {
        Remove-Item $path -Force -ErrorAction SilentlyContinue
        throw "$name download was unexpectedly small."
    }
}

Write-Host "Preparing optional enhanced-temperature resources..."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Get-File $pawnUrl $installer "PawnIO $pawnIoVersion installer"
Get-File $intelUrl $intel "IntelMSR PawnIO module"
Get-File $amdUrl $amd "AMDFamily17 PawnIO module"

$actual = (Get-FileHash -Path $installer -Algorithm SHA256).Hash.ToUpperInvariant()
if ($actual -ne $expectedPawnSha256) {
    throw "PawnIO installer SHA256 mismatch. Expected $expectedPawnSha256 but got $actual"
}

Write-Host "  Sensor resources ready."
