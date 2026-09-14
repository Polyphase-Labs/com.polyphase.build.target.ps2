#!/bin/sh
# Hand the running PS2 game back to ps2link, so you can push a new build.
#
# Shorthand for: ./ps2reload.sh ps2link
# Once ps2link is back up, ./ps2run.sh launches the next build as usual.
#
# Requires the 'Remote Reload' Target Option to be CHECKED - compiled out by
# default.
#
# Note there is usually no need to go via ps2link at all: ./ps2reload.sh with no
# arguments relaunches the freshly built ELF directly, one step instead of two.
# Use this when you want ps2link itself back.
#
# Usage: ./ps2link.sh [path-to-ps2link.elf]
#        PS2LINK_ELF=mass:/PS2LINK/ps2link.elf ./ps2link.sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
target="${1:-${PS2LINK_ELF:-mc0:/ps2link_1.2/PS2LINK.ELF}}"

"$script_dir/ps2reload.sh" "$target"

echo
echo "If ps2link does not come back, it is almost certainly the path."
echo "Check where ps2link.elf actually lives and pass it:"
echo "    ./ps2link.sh mc0:/ps2link_1.2/PS2LINK.ELF"
echo "    ./ps2link.sh mass:/PS2LINK/ps2link.elf"
echo "or set PS2LINK_ELF once. Failing that, ./ps2reload.sh osdsys reaches the"
echo "BIOS browser, which never depends on host: still working."
