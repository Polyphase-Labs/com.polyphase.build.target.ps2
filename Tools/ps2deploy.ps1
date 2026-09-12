<#
.SYNOPSIS
    Stage a standalone build for running on the console WITHOUT ps2link.

.DESCRIPTION
    Produces a folder whose CONTENTS go at the ROOT of a USB stick or memory
    card. Root matters: the engine resolves assets as "<bootdevice>/<relative
    path>", so booting mass:/GAME.ELF makes it look for mass:/BuildTarget-PS2/
    Assets/... - putting the asset tree in a subfolder silently breaks loading.

    The ELF is stripped on the way: the linker leaves ~155 MB of debug info in
    it, of which only ~7.8 MB is actually loadable. Copying the unstripped one
    works but wastes minutes of USB write time per iteration.

.PARAMETER Dest
    Optional drive or folder to copy into, e.g. E:\ . Without it the staged
    folder is left in Build\PS2\Deploy for you to copy by hand.

.EXAMPLE
    .\ps2deploy.ps1
    .\ps2deploy.ps1 -Dest E:\
#>
[CmdletBinding()]
param(
    [string]$Dest
)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$buildDir    = Join-Path $projectRoot 'Build\PS2'
$deployDir   = Join-Path $buildDir 'Deploy'
$projName    = Split-Path $projectRoot -Leaf
$elf         = Join-Path $buildDir "$projName.elf"

if (-not (Test-Path $elf)) { throw "No ELF at '$elf'. Build the PS2 target first." }

# --- strip -------------------------------------------------------------------
# ps2dev's binaries don't run natively on Windows (they exit 0xC000007B), so the
# strip goes through WSL like the rest of the toolchain.
function ToWslPath([string]$p) {
    $full = (Resolve-Path -LiteralPath $p -ErrorAction SilentlyContinue)
    if (-not $full) { $full = $p }
    $s = ("$full").Replace([char]92, [char]47)   # no regex: avoids escaping a backslash
    if ($s -match '^([A-Za-z]):(.*)$') { return "/mnt/$($Matches[1].ToLower())$($Matches[2])" }
    return $s
}

if (Test-Path $deployDir) { Remove-Item $deployDir -Recurse -Force }
New-Item -ItemType Directory -Path $deployDir | Out-Null

$outElf = Join-Path $deployDir "$projName.ELF"
$cmd = "export PATH=`$PATH:/usr/local/ps2dev/ee/bin; mips64r5900el-ps2-elf-strip -o '$(ToWslPath $outElf)' '$(ToWslPath $elf)'"
& wsl.exe -e bash -lc $cmd
if (-not (Test-Path $outElf)) { throw "strip failed - is WSL + ps2dev available?" }

$before = (Get-Item $elf).Length / 1MB
$after  = (Get-Item $outElf).Length / 1MB
Write-Host ("ELF    : {0:N1} MB -> {1:N1} MB stripped" -f $before, $after)

# --- content -----------------------------------------------------------------
foreach ($item in @($projName, 'Engine', 'Config.ini', "$projName.octp")) {
    $src = Join-Path $buildDir $item
    if (Test-Path $src) {
        Copy-Item $src -Destination $deployDir -Recurse -Force
        Write-Host "Staged : $item"
    } else {
        Write-Warning "Missing (skipped): $item"
    }
}

# A leftover marker would make a Remote Reload build bounce on its first frame.
Remove-Item (Join-Path $deployDir 'reload.cmd') -Force -ErrorAction SilentlyContinue

# ---- Lua scripts ------------------------------------------------------------
# Nothing is embedded in the ELF despite the profile's "embedded" flag:
# gEmbeddedScripts lives in .bss (zeroed) and Generated/EmbeddedScripts.cpp is an
# empty 136-byte stub, so gNumEmbeddedScripts is 0. Scripts are loaded from DISK.
#
# The build does not stage <project>/Scripts into Build/PS2 at all, so they have
# to be taken from the project root. Without them nothing scripted runs - no
# rotation, no menu input - and the engine reports no error, because a missing
# script is not fatal to it.
#
# Staged to BOTH candidate roots: the engine resolves some paths relative to the
# device root and others relative to the project folder, and 22 small files are
# not worth being clever about.
$srcScripts = Join-Path $projectRoot 'Scripts'
if (Test-Path $srcScripts) {
    $n = (Get-ChildItem $srcScripts -Recurse -File -Filter *.lua).Count
    Copy-Item $srcScripts -Destination $deployDir -Recurse -Force
    Copy-Item $srcScripts -Destination (Join-Path $deployDir $projName) -Recurse -Force
    Write-Host "Staged : Scripts/ ($n .lua, to both roots)"
} else {
    Write-Warning "No Scripts/ folder at $srcScripts - scripted behaviour will not run"
}

# Prune Packages/ - but do NOT drop it.
#
# It was dropped wholesale on the first attempt because a full ps2link run's log
# never mentioned "Packages/". That check was worthless: the engine logs script
# NAMES, not paths. Removing the tree took the video plugin's Scripts/*.lua and
# its package.json with it, and on hardware the whole startup chain went missing
# - no "Loading script: EngineStartup", no "RuntimePluginManager: Registered
# plugin", and nothing rotating because no Lua ran at all.
#
# So keep the runtime parts (package.json, Scripts/, Assets/) and prune only
# what is genuinely build-time. External/ alone is ~250 MB of ffmpeg.
$pkgRoot = Join-Path $deployDir "$projName\Packages"
if (Test-Path $pkgRoot) {
    $before = (Get-ChildItem $pkgRoot -Recurse -File | Measure-Object Length -Sum).Sum / 1MB

    # Directories that are build inputs, never runtime data.
    foreach ($d in @('External', 'Build', 'Intermediate', 'Docs', '.git', 'obj', 'bin')) {
        Get-ChildItem $pkgRoot -Recurse -Directory -Filter $d -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
    }
    # Source code and build artefacts. Native addons are compiled INTO the ELF.
    # Source/Assets is kept: some packages ship runtime content under it.
    Get-ChildItem $pkgRoot -Recurse -File -ErrorAction SilentlyContinue | Where-Object {
        $_.Extension -in @('.h','.hpp','.c','.cpp','.cc','.inl','.a','.lib','.dll','.so',
                           '.dylib','.exe','.pdb','.obj','.vcxproj','.sln','.md') -and
        $_.FullName -notlike ([char]42 + [char]92 + [char]65 + [char]115 + [char]115 + [char]101 + [char]116 + [char]115 + [char]92 + [char]42)
    } | Remove-Item -Force -ErrorAction SilentlyContinue

    $after = (Get-ChildItem $pkgRoot -Recurse -File -ErrorAction SilentlyContinue |
              Measure-Object Length -Sum).Sum / 1MB
    $lua = (Get-ChildItem $pkgRoot -Recurse -File -Filter *.lua -ErrorAction SilentlyContinue).Count
    Write-Host ("Pruned : Packages/ {0:N0} MB -> {1:N1} MB (kept {2} .lua)" -f $before, $after, $lua)
}

$size = (Get-ChildItem $deployDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Total  : {0:N1} MB in {1}" -f $size, $deployDir)

if ($Dest) {
    if (-not (Test-Path $Dest)) { throw "Destination '$Dest' not found." }
    Write-Host "Copying to $Dest ..."
    Copy-Item (Join-Path $deployDir '*') -Destination $Dest -Recurse -Force
    Write-Host "Done. Boot $projName.ELF from the device (wLaunchELF: mass:/$projName.ELF)."
} else {
    Write-Host ""
    Write-Host "Copy the CONTENTS of that folder to the ROOT of your USB stick or"
    Write-Host "memory card, then run $projName.ELF from wLaunchELF."
}
