#!/bin/sh
# HKPSP player build on Linux: copied to the root of the Linux release bundle next to
# runtime/python. Builds the game data from your Steam copy of Hollow Knight; pass --game
# "<folder>" when Steam is not found, --keep to keep the work files, --check to only check.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$here/runtime/python/bin/python3" -I "$here/tools/psp/hkpsp_build.py" "$@"
