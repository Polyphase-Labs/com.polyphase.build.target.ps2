#!/bin/sh
# Remotely hand the running PS2 game over to another ELF - no reset button.
#
# Drops a 'reload.cmd' marker into Build/PS2 (what host: maps to while ps2client
# runs). Within a second the game reads the path from it, deletes the marker and
# LoadExecPS2's the target.
#
# Requires the 'Remote Reload' Target Option to be CHECKED - it is compiled out
# by default.
#
# Usage: ./ps2reload.sh [game|ps2link|menu|osdsys|<path>]   (default: game)
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
build_dir="$project_root/Build/PS2"

[ -d "$build_dir" ] || { echo "No build directory at '$build_dir'." >&2; exit 1; }

elf_name="$(basename "$project_root").elf"

case "${1:-game}" in
    game)           target="host:$elf_name" ;;
    ps2link)        target="${PS2LINK_ELF:-mc0:/ps2link_1.2/PS2LINK.ELF}" ;;
    menu|fmcb)      target="${PS2_MENU_ELF:-mc0:/SYS_OSDMENU/osdmenu.elf}" ;;
    osdsys|reboot)  target="rom0:OSDSYS" ;;
    *)              target="$1" ;;
esac

# A host: open of a MISSING file creates a directory of that name under ps2link,
# after which every write here fails and the console can never read the marker.
if [ -d "$build_dir/reload.cmd" ]; then
    rmdir "$build_dir/reload.cmd" 2>/dev/null || rm -rf "$build_dir/reload.cmd"
    echo "Cleared a stray 'reload.cmd' DIRECTORY (ps2link makes one when the console opens a missing path)."
fi

# printf, not echo: no trailing newline, no shell-specific escape handling.
printf '%s' "$target" > "$build_dir/reload.cmd"

echo "reload.cmd -> $target"
echo "Dropped at : $build_dir/reload.cmd"
echo
echo "The game polls once a second. If nothing happens, check that:"
echo "  - the 'Remote Reload' Target Option was checked for this build"
echo "  - ps2client is running with host: mapped to Build/PS2"
echo "  - the log does not say LoadExecPS2 returned (then try: $0 osdsys)"
