#!/bin/sh
# Launch the staged PS2 ELF on hardware over ps2link.
#
# ps2client serves host: from ITS OWN working directory, so this cd's into
# Build/PS2 first. Get that wrong and the game boots but every asset load fails.
#
# Usage: ./ps2run.sh [ip] [elf]
#        PS2_IP=192.168.1.50 ./ps2run.sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
build_dir="$project_root/Build/PS2"

[ -d "$build_dir" ] || { echo "No build directory at '$build_dir'." >&2; exit 1; }

ip="${1:-${PS2_IP:-}}"
elf="${2:-$(basename "$project_root").elf}"

[ -f "$build_dir/$elf" ] || { echo "ELF not found: $build_dir/$elf" >&2; exit 1; }

command -v ps2client >/dev/null 2>&1 || {
    echo "ps2client not on PATH." >&2; exit 1; }

# Pre-create the reload marker holding 'none' - see ps2run.ps1 for why a
# missing file is dangerous here (ps2link turns it into a directory).
[ -d "$build_dir/reload.cmd" ] && rm -rf "$build_dir/reload.cmd"
printf 'none' > "$build_dir/reload.cmd"

echo "ELF   : $elf"
echo "host: = $build_dir"
if [ -n "$ip" ]; then
    echo "Target: $ip"
    cd "$build_dir" && exec ps2client -h "$ip" execee "host:$elf"
else
    echo "Target: (ps2client default - pass an ip or set PS2_IP)"
    cd "$build_dir" && exec ps2client execee "host:$elf"
fi
