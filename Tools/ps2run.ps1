<#
.SYNOPSIS
    Launch the staged PS2 ELF on hardware over ps2link.

.DESCRIPTION
    ps2client serves `host:` from ITS OWN working directory, so this script cd's
    into Build/PS2 before launching. Get that wrong and the game boots but every
    asset load fails.

.PARAMETER Ip
    Console IP. Falls back to $env:PS2_IP, then to ps2client's own default.

.PARAMETER Elf
    ELF to run. Defaults to <ProjectName>.elf in Build/PS2.

.EXAMPLE
    .\ps2run.ps1 -Ip 192.168.1.50
    $env:PS2_IP = '192.168.1.50'; .\ps2run.ps1
#>
[CmdletBinding()]
param(
    [string]$Ip  = $env:PS2_IP,
    [string]$Elf
)

$ErrorActionPreference = 'Stop'

# Tools/ -> com.polyphase.build.target.ps2/ -> Packages/ -> <project root>
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$buildDir    = Join-Path $projectRoot 'Build\PS2'

if (-not (Test-Path $buildDir)) {
    throw "No build directory at '$buildDir'. Build the PS2 target first."
}

if (-not $Elf) { $Elf = "$(Split-Path $projectRoot -Leaf).elf" }
$elfPath = Join-Path $buildDir $Elf
if (-not (Test-Path $elfPath)) {
    throw "ELF not found: '$elfPath'. Build the PS2 target first."
}

$stamp = (Get-Item $elfPath).LastWriteTime
$age   = [int]((Get-Date) - $stamp).TotalMinutes
Write-Host "ELF   : $Elf  ($('{0:N1}' -f ((Get-Item $elfPath).Length / 1MB)) MB, built $age min ago)"
if ($age -gt 10) {
    # A stale binary has cost this project several debugging cycles. Say so.
    Write-Warning "That ELF is $age minutes old - is it the build you meant to test?"
}

$ps2client = Get-Command ps2client -ErrorAction SilentlyContinue
if (-not $ps2client) {
    if ($env:PS2DEV -and (Test-Path (Join-Path $env:PS2DEV 'bin\ps2client.exe'))) {
        $ps2client = Join-Path $env:PS2DEV 'bin\ps2client.exe'
    } else {
        throw "ps2client not found on PATH or under `$env:PS2DEV\bin."
    }
} else {
    $ps2client = $ps2client.Source
}

$hostArgs = @()
if ($Ip) { $hostArgs += @('-h', $Ip); Write-Host "Target: $Ip" }
else     { Write-Host "Target: (ps2client default - pass -Ip or set `$env:PS2_IP to override)" }

Write-Host "host: = $buildDir"
Write-Host ''

# Pre-create the reload marker holding 'none'. The console polls this file once
# a second when Remote Reload is enabled, and a host: open of a file that is not
# there makes ps2link create a DIRECTORY of that name - which then blocks both
# the console's read and the PC's write for good. Keeping it present avoids the
# missing-file case entirely.
$marker = Join-Path $buildDir 'reload.cmd'
if (Test-Path -LiteralPath $marker -PathType Container) {
    Remove-Item -LiteralPath $marker -Recurse -Force
}
Set-Content -LiteralPath $marker -Value 'none' -Encoding ascii -NoNewline

Push-Location $buildDir
try {
    & $ps2client @hostArgs execee "host:$Elf"
} finally {
    Pop-Location
}
