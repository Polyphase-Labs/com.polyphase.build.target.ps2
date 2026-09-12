#!/bin/sh
# Tail the PS2 runtime log written over host:.
#
# Usage: ./ps2log.sh            follow everything
#        ./ps2log.sh profile    only the per-second [PS2] frame profile lines
#        ./ps2log.sh packaged   read Packaged/homebrew.ps2 instead of Build/PS2
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)

dir="$project_root/Build/PS2"
mode="${1:-all}"
[ "$mode" = "packaged" ] && dir="$project_root/Packaged/homebrew.ps2"

log="$dir/ps2-addon.log"
[ -f "$log" ] || { echo "No log at '$log' - has the game run yet?" >&2; exit 1; }

if [ "$mode" = "profile" ]; then
    tail -n 40 -f "$log" | grep -E '\[PS2\] (VU1|EE )'
else
    tail -n 40 -f "$log"
fi
