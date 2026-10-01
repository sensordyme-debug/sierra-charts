<#
.SYNOPSIS
  Syntax-checks every NQ Edge study source against the Sierra Chart headers.

.DESCRIPTION
  Finds a C++ compiler (portable llvm-mingw in tools/, clang++/g++ on PATH, or MSVC via
  vswhere) and runs a syntax-only compile of src/sierra/*.cpp with -I third_party/sierra.
  Exit code 0 = clean, 1 = errors. Pass -Bootstrap to download llvm-mingw into tools/ if no
  compiler is found (about 190 MB, no admin rights needed).

.EXAMPLE
  .\scripts\check.ps1
  .\scripts\check.ps1 -Bootstrap
#>
[CmdletBinding()]
param(
    [switch]$Bootstrap,
    [string]$Source = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$inc  = Join-Path $root "third_party\sierra"
$srcDir = Join-Path $root "src\sierra"

if (-not (Test-Path (Join-Path $inc "sierrachart.h"))) {
    Write-Host "ERROR: third_party/sierra/sierrachart.h not found. Copy C:\SierraChart\ACS_Source\* into third_party\sierra\." -ForegroundColor Red
    exit 1
}

function Find-Clang {
    $cands = @()
    $cands += Get-ChildItem -Path (Join-Path $root "tools") -Directory -Filter "llvm-mingw*" -ErrorAction SilentlyContinue |
              ForEach-Object { Join-Path $_.FullName "bin\clang++.exe" }
    $c = Get-Command clang++ -ErrorAction SilentlyContinue
    if ($c) { $cands += $c.Source }
    foreach ($p in $cands) { if ($p -and (Test-Path $p)) { return $p } }
    return $null
}

function Find-Gpp {
    $g = Get-Command g++ -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }
    return $null
}

function Find-MSVC {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $vsPath) { return $null }
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $vcvars) { return $vcvars }
    return $null
}

function Bootstrap-Clang {
    $tools = Join-Path $root "tools"
    New-Item -ItemType Directory -Force -Path $tools | Out-Null
    Write-Host "Downloading portable llvm-mingw (clang) into tools\ ..." -ForegroundColor Cyan
    $api = Invoke-RestMethod -Uri "https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest" -UseBasicParsing
    $asset = $api.assets | Where-Object { $_.name -like "*ucrt-x86_64.zip" } | Select-Object -First 1
    if (-not $asset) { throw "Could not find llvm-mingw ucrt-x86_64 zip in the latest release." }
    $zip = Join-Path $tools "llvm-mingw.zip"
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $tools -Force
    Remove-Item $zip -Force
    Write-Host "Done." -ForegroundColor Green
}

$files = @()
if ($Source) { $files = @((Resolve-Path $Source).Path) }
else { $files = Get-ChildItem -Path $srcDir -Filter "*.cpp" | ForEach-Object { $_.FullName } }
if ($files.Count -eq 0) { Write-Host "No sources in src\sierra." -ForegroundColor Yellow; exit 0 }

$clang = Find-Clang
if (-not $clang -and $Bootstrap) { Bootstrap-Clang; $clang = Find-Clang }
$gpp = $null; $msvc = $null
if (-not $clang) { $gpp = Find-Gpp }
if (-not $clang -and -not $gpp) { $msvc = Find-MSVC }

if (-not $clang -and -not $gpp -and -not $msvc) {
    Write-Host "ERROR: no C++ compiler found. Run: .\scripts\check.ps1 -Bootstrap   (downloads portable clang into tools\)" -ForegroundColor Red
    exit 1
}

$failed = $false
foreach ($f in $files) {
    Write-Host ("Checking " + (Split-Path -Leaf $f) + " ...") -ForegroundColor Cyan
    if ($clang) {
        & $clang -fsyntax-only -std=c++17 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX_NOT_DEFINED `
            -Wall -Wextra -Wno-unused-parameter -Wno-unused-value -Wno-unused-function -Wno-missing-field-initializers `
            -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable -Wno-deprecated-declarations `
            -isystem "$inc" "$f"
        if ($LASTEXITCODE -ne 0) { $failed = $true }
    }
    elseif ($gpp) {
        & $gpp -fsyntax-only -std=c++17 -D_CRT_SECURE_NO_WARNINGS -Wall -Wno-unused-value -isystem "$inc" "$f"
        if ($LASTEXITCODE -ne 0) { $failed = $true }
    }
    else {
        $cmd = "`"$msvc`" >nul && cl /nologo /Zs /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS /I `"$inc`" `"$f`""
        cmd /c $cmd
        if ($LASTEXITCODE -ne 0) { $failed = $true }
    }
}

if ($failed) { Write-Host "SYNTAX CHECK FAILED" -ForegroundColor Red; exit 1 }
Write-Host "SYNTAX CHECK OK" -ForegroundColor Green
exit 0
