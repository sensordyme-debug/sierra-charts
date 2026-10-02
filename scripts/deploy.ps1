<#
.SYNOPSIS
  Copies the NQ Edge Suite into a Sierra Chart installation.

.DESCRIPTION
  - src\sierra\*.cpp              -> <SierraPath>\ACS_Source\
  - sierra\chartbooks\*.Cht       -> <SierraPath>\Data\
  - sierra\studycollections\*     -> <SierraPath>\Data\
  - config\NQEdge_weights.txt     -> <SierraPath>\Data\   (only if not present, unless -ForceWeights)
  Then prints the exact clicks to build and load.

.EXAMPLE
  .\scripts\deploy.ps1
  .\scripts\deploy.ps1 -SierraPath "D:\SierraChart" -ForceWeights
#>
[CmdletBinding()]
param(
    [string]$SierraPath = "C:\SierraChart",
    [switch]$ForceWeights,
    [switch]$SkipCheck
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not (Test-Path $SierraPath)) { Write-Host "ERROR: Sierra Chart folder not found: $SierraPath" -ForegroundColor Red; exit 1 }
$acs  = Join-Path $SierraPath "ACS_Source"
$data = Join-Path $SierraPath "Data"
$dataFolderFile = Join-Path $SierraPath "DataFilesFolder.txt"
if (Test-Path $dataFolderFile) {
    $custom = (Get-Content $dataFolderFile -Raw).Trim()
    if ($custom -and (Test-Path $custom)) { $data = $custom }
}
New-Item -ItemType Directory -Force -Path $acs, $data | Out-Null

if (-not $SkipCheck) {
    & (Join-Path $root "scripts\check.ps1")
    if ($LASTEXITCODE -ne 0) { Write-Host "Deploy aborted: syntax check failed." -ForegroundColor Red; exit 1 }
}

$copied = @()
Get-ChildItem (Join-Path $root "src\sierra") -Filter "*.cpp" | ForEach-Object {
    Copy-Item $_.FullName -Destination $acs -Force; $copied += "ACS_Source\" + $_.Name
}
$cb = Join-Path $root "sierra\chartbooks"
if (Test-Path $cb) { Get-ChildItem $cb -Filter "*.Cht" | ForEach-Object { Copy-Item $_.FullName -Destination $data -Force; $copied += "Data\" + $_.Name } }
$sc = Join-Path $root "sierra\studycollections"
if (Test-Path $sc) { Get-ChildItem $sc -File -Filter "*.StdyCollct" | ForEach-Object { Copy-Item $_.FullName -Destination $data -Force; $copied += "Data\" + $_.Name } }
$w = Join-Path $root "config\NQEdge_weights.txt"
if (Test-Path $w) {
    $dst = Join-Path $data "NQEdge_weights.txt"
    if ($ForceWeights -or -not (Test-Path $dst)) { Copy-Item $w -Destination $dst -Force; $copied += "Data\NQEdge_weights.txt" }
    else { Write-Host "Kept existing Data\NQEdge_weights.txt (use -ForceWeights to overwrite)." -ForegroundColor Yellow }
}

Write-Host ""
Write-Host "Deployed:" -ForegroundColor Green
$copied | ForEach-Object { Write-Host "  $_" }
Write-Host ""
Write-Host "Next clicks in Sierra Chart:" -ForegroundColor Cyan
Write-Host "  1. Analysis >> Build Custom Studies DLL >> Remote Build - Release"
Write-Host "       select  NQEdgeSuite.cpp  >> Build. Wait for 'Build successful' in the Build window."
Write-Host "  2. On the price chart: Analysis >> Studies >> Add Custom Study >> NQ Edge Suite >> 'NQ Edge Terminal'"
Write-Host "       (one study is the whole system; remove any older NQ Edge studies from the chart)"
Write-Host "  3. In the Terminal inputs set Chart Number: YM / NYSE TICK / Mega Cap 1-2 from the other charts' title bars."
Write-Host "  4. Chart >> Chart Settings: Fill Space 30-40 bars. File >> Save Chartbook."
Write-Host "  5. See docs\SETUP.md for settings checks (1-tick storage, NY time zone) and docs\VISUAL_GUIDE.md for what you see."
