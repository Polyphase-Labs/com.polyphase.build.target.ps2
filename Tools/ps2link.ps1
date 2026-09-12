<#
.SYNOPSIS
    Hand the running PS2 game back to ps2link, so you can push a new build.

.DESCRIPTION
    Shorthand for `ps2reload.ps1 -Target ps2link`. Once ps2link is back up,
    `ps2run.ps1` will launch the next build as usual.

    Requires the 'Remote Reload' Target Option to be CHECKED in the build
    profile - it is compiled out by default.

    Note there is usually no need to go via ps2link at all: `ps2reload.ps1`
    with no arguments relaunches the freshly built ELF directly, which is one
    step rather than two. Use this when you want ps2link itself back - to run a
    different homebrew, or because a host: target failed to load.

.PARAMETER Path
    Where ps2link.elf lives on the console. Defaults to $env:PS2LINK_ELF, then
    to mc0:/ps2link_1.2/PS2LINK.ELF.

.EXAMPLE
    .\ps2link.ps1
    .\ps2link.ps1 -Path mass:/APPS/PS2LINK/ps2link.elf
#>
[CmdletBinding()]
param(
    [string]$Path
)

$ErrorActionPreference = 'Stop'

if (-not $Path) {
    $Path = if ($env:PS2LINK_ELF) { $env:PS2LINK_ELF }
            else { 'mc0:/ps2link_1.2/PS2LINK.ELF' }
}

& (Join-Path $PSScriptRoot 'ps2reload.ps1') -Target $Path

Write-Host ''
Write-Host "If ps2link does not come back, it is almost certainly the path."
Write-Host "Check where ps2link.elf actually lives and pass it:"
Write-Host "    .\ps2link.ps1 -Path mc0:/ps2link_1.2/PS2LINK.ELF"
Write-Host "    .\ps2link.ps1 -Path mass:/PS2LINK/ps2link.elf"
Write-Host "or set `$env:PS2LINK_ELF once. Failing that, .\ps2reload.ps1 -Target osdsys"
Write-Host "reaches the BIOS browser, which never depends on host: still working."
