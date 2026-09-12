<#
.SYNOPSIS
    Remotely hand the running PS2 game over to another ELF - no reset button.

.DESCRIPTION
    Drops a 'reload.cmd' marker into Build/PS2 (which is what `host:` maps to
    while ps2client is running). Within a second the game reads the path out of
    it, deletes the marker, stops the audio mixer and LoadExecPS2's the target.

    Requires the 'Remote Reload' Target Option to be CHECKED in the build
    profile - it is compiled out by default.

.PARAMETER Target
    game    - relaunch the freshly built ELF (hot reload). Default.
    ps2link - back to ps2link, then run ps2run.ps1 again.
    menu    - the FreeMcBoot OSD menu (mc0:/SYS_OSDMENU/osdmenu.elf), from which
              you can launch anything. 'fmcb' is an alias. Override with
              $env:PS2_MENU_ELF.
    osdsys  - the bare BIOS browser; the only target that does not need ps2link's
              IOP modules to still be serving host:, so it is the last resort.
    <path>  - anything else is passed through verbatim, e.g. mc0:/APPS/FOO.ELF

.EXAMPLE
    .\ps2reload.ps1                 # relaunch the new build
    .\ps2reload.ps1 -Target osdsys  # bail out to the browser
#>
[CmdletBinding()]
param(
    [string]$Target = 'game'
)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$buildDir    = Join-Path $projectRoot 'Build\PS2'
if (-not (Test-Path $buildDir)) {
    throw "No build directory at '$buildDir'."
}

$elfName = "$(Split-Path $projectRoot -Leaf).elf"

switch ($Target.ToLowerInvariant()) {
    'game'    { $path = "host:$elfName" }
    'ps2link' { $path = if ($env:PS2LINK_ELF) { $env:PS2LINK_ELF }
                       else { 'mc0:/ps2link_1.2/PS2LINK.ELF' } }
    'menu'    { $path = if ($env:PS2_MENU_ELF) { $env:PS2_MENU_ELF }
                       else { 'mc0:/SYS_OSDMENU/osdmenu.elf' } }
    'fmcb'    { $path = if ($env:PS2_MENU_ELF) { $env:PS2_MENU_ELF }
                       else { 'mc0:/SYS_OSDMENU/osdmenu.elf' } }
    'osdsys'  { $path = 'rom0:OSDSYS' }
    'reboot'  { $path = 'rom0:OSDSYS' }
    default   { $path = $Target }
}

$marker = Join-Path $buildDir 'reload.cmd'

# A host: open of a MISSING file creates a directory of that name under ps2link.
# Once that happens every write here fails with 'access denied' and the console
# can never read the marker again - the feature silently disables itself. Clear
# it rather than making the user work out why nothing happens.
if (Test-Path -LiteralPath $marker -PathType Container) {
    Remove-Item -LiteralPath $marker -Recurse -Force
    Write-Host "Cleared a stray 'reload.cmd' DIRECTORY (ps2link creates one when the console opens a missing path)."
}

# ASCII, no BOM: the runtime reads these bytes as a path. A UTF-8 BOM would be
# prepended to it and the open would fail.
Set-Content -LiteralPath $marker -Value $path -Encoding ascii -NoNewline

Write-Host "reload.cmd -> $path"
Write-Host "Dropped at : $marker"
Write-Host ''
Write-Host "The game polls once a second. If nothing happens, check that:"
Write-Host "  - the 'Remote Reload' Target Option was checked for this build"
Write-Host "  - ps2client is running with host: mapped to Build\PS2"
Write-Host "  - the log does not say LoadExecPS2 returned (then try -Target osdsys)"
