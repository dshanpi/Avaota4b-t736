#!/bin/sh
# Run from the independently deployed project on the Linux build host.
# Fetch only: this does not claim that a T736 board port already exists.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
if [ "$(uname -s)" != Linux ]; then
    printf '%s\n' 'This preparation entry point requires a Linux build host.' >&2
    exit 1
fi
command -v python3 >/dev/null
command -v git >/dev/null
python3 - "$project_dir" <<'PY'
import sys
from pathlib import Path
root = Path(sys.argv[1])
sys.path.insert(0, str(root / 'tools'))
from workspace_policy import outside_tina
outside_tina(root)
for name in ('sources', 'build', 'output', 'logs'):
    outside_tina(root / name).mkdir(exist_ok=True)
print('Independent mainline workspace:', root)
PY
python3 "$project_dir/tools/prepare_sources.py" --fetch --destination "$project_dir/sources"
printf '%s\n' 'Pinned source checkouts ready. T736-specific boot/kernel implementation is still required before building a flash image.'
