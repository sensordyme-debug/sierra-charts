<#
.SYNOPSIS
  Watches src\ and re-runs deploy.ps1 (syntax check + copy) on every save.

.EXAMPLE
  .\scripts\watch.ps1
  .\scripts\watch.ps1 -SierraPath "D:\SierraChart"
#>
[CmdletBinding()]
param(
    [string]$SierraPath = "C:\SierraChart",
    [int]$DebounceMs = 800
)
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$watchPath = Join-Path $root "src"
$deploy = Join-Path $root "scripts\deploy.ps1"

Write-Host "Watching $watchPath  (Ctrl+C to stop). Edit >> save >> Sierra: Analysis >> Build Custom Studies DLL >> Remote Build." -ForegroundColor Cyan

$fsw = New-Object System.IO.FileSystemWatcher
$fsw.Path = $watchPath
$fsw.Filter = "*.cpp"
$fsw.IncludeSubdirectories = $true
$fsw.EnableRaisingEvents = $true

$script:pending = $false
$script:last = Get-Date
$handler = { $script:pending = $true; $script:last = Get-Date }
$subs = @()
$subs += Register-ObjectEvent $fsw Changed -Action $handler
$subs += Register-ObjectEvent $fsw Created -Action $handler
$subs += Register-ObjectEvent $fsw Renamed -Action $handler

try {
    while ($true) {
        Start-Sleep -Milliseconds 200
        if ($script:pending -and ((Get-Date) - $script:last).TotalMilliseconds -ge $DebounceMs) {
            $script:pending = $false
            Write-Host ("`n[" + (Get-Date -Format "HH:mm:ss") + "] change detected -> deploy") -ForegroundColor Yellow
            & $deploy -SierraPath $SierraPath
        }
    }
}
finally {
    $subs | ForEach-Object { Unregister-Event -SubscriptionId $_.Id -ErrorAction SilentlyContinue }
    $fsw.Dispose()
}
