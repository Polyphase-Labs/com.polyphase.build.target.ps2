<#
.SYNOPSIS
    Tail the PS2 runtime log written over host:.

.PARAMETER Profile
    Show only the one-line-per-second [PS2] frame profile lines.

.PARAMETER Packaged
    Read the packaged run's log (Packaged/homebrew.ps2) instead of Build/PS2.
#>
[CmdletBinding()]
param(
    [switch]$Profile,
    [switch]$Packaged,
    [int]$Tail = 40
)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$dir = if ($Packaged) { Join-Path $projectRoot 'Packaged\homebrew.ps2' }
       else           { Join-Path $projectRoot 'Build\PS2' }
$log = Join-Path $dir 'ps2-addon.log'

if (-not (Test-Path $log)) { throw "No log at '$log' - has the game run yet?" }

if ($Profile) {
    Get-Content -LiteralPath $log -Wait -Tail $Tail |
        Where-Object { $_ -match '\[PS2\] (VU1|EE )' }
} else {
    Get-Content -LiteralPath $log -Wait -Tail $Tail
}
