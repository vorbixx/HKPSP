#!/usr/bin/env bash
# Reproduce PSPLINK using the installed WSL pspdev; never install downloaded code.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
commit=7a0035d940d8484a363a9d5cf55fa2cc067ab13e
archive_sha=b82a7a35fafd7cc8c127ca2a54d04c2f1a99f8a03f872c8bad41b094b51993b1
url="https://codeload.github.com/pspdev/psplinkusb/tar.gz/$commit"
export PSPDEV="$HOME/pspdev"
export PATH="$PSPDEV/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
mkdir -p "$repo/build/psp/psplink-src"
# Each invocation downloads into a new empty directory, including reproduction checks.
work="$(mktemp -d "$repo/build/psp/psplink-src/$commit.XXXXXX")"
echo "Source/build log: $work"
curl --fail --location --silent --show-error "$url" -o "$work/source.tar.gz"
printf '%s  %s\n' "$archive_sha" "$work/source.tar.gz" | sha256sum --check
tar -xzf "$work/source.tar.gz" -C "$work"
source="$work/psplinkusb-$commit"
make -C "$source" -f Makefile.psp release > "$work/build.log" 2>&1 || {
    tail -n 40 "$work/build.log"; exit 1;
}
stage="$repo/build/psp/psplink/PSP/GAME/PSPLINK"
mkdir -p "$stage"
python3 -I - "$stage" "$commit" "$archive_sha" "$url" "$work" "$source/release/psplink" <<'PY'
import hashlib
import json
import shutil
from pathlib import Path
import sys
stage = Path(sys.argv[1])
expected = {'EBOOT.PBP', 'psplink.prx', 'psplink_user.prx', 'usbhostfs.prx', 'usbgdb.prx', 'psplink.ini'}
source = Path(sys.argv[6])
files = {name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in sorted(expected)}
if {p.name for p in stage.iterdir()} - expected:
    raise SystemExit('Unexpected PSPLINK staging contents')
manifest = stage.parents[2] / 'provenance.json'
if manifest.exists() and json.loads(manifest.read_text())['files'] != files:
    raise SystemExit('PSPLINK reproduction hashes differ from previous staging')
for name, digest in files.items():
    shutil.copyfile(source / name, stage / name)
    if hashlib.sha256((stage / name).read_bytes()).hexdigest() != digest:
        raise SystemExit('PSPLINK staged hash mismatch: ' + name)
manifest.write_text(json.dumps(dict(commit=sys.argv[2], archive_sha256=sys.argv[3],
                                  url=sys.argv[4], build_directory=sys.argv[5], files=files), indent=2) + '\n')
print(json.dumps(files, indent=2))
PY
