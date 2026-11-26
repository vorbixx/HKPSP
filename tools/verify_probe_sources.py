"""Read-only hash audit of probe inputs against Steam and the archived source."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from assetripper_client import ROOT


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(provenance, steam, archived):
    steam=steam.resolve();archived=archived.resolve()
    # PC2 has only the archived copy (with its own app manifest), no Steam install: the copy is then
    # checked against its manifest build and the provenance hashes alone (2026-10-26: every original
    # capture there failed on the missing Steam path).
    with_steam = steam.is_dir()
    manifest = (steam.parents[1] if with_steam else archived.parent) / "appmanifest_367520.acf"
    build = re.search(r'"buildid"\s+"(\d+)"', manifest.read_text())[1]
    if build != "22529139":
        raise ValueError(f"Steam build changed: {build}; re-inventory before using these observations")
    inputs = json.loads(provenance.read_text())["source_sha256"]
    files = []
    for name, expected in inputs.items():
        path = (ROOT / name.replace("\\", "/")).resolve()
        if not path.is_relative_to(archived):
            continue
        relative = path.relative_to(archived)
        checked = [path, steam / relative] if with_steam else [path]  # archive first, then Steam
        if any(sha(source) != expected for source in checked):
            raise ValueError(f"Input changed: {relative}")
        files.append(dict(file=relative.as_posix(), paths=[str(source) for source in reversed(checked)],
                          sha256=expected, matches=True))
    if not files:
        raise ValueError("No archived player inputs in provenance")
    method = ("Read-only Steam/archive/probe-provenance comparison" if with_steam else
              "Read-only archive/probe-provenance comparison (no Steam install on this PC)")
    return dict(build=build, method=method, files=files, validated=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provenance", type=Path, default=ROOT / "_private/crawler-fsm-probe/results/provenance.json")
    parser.add_argument("--steam", type=Path, default=Path("C:/Program Files (x86)/Steam/steamapps/common/Hollow Knight"))
    parser.add_argument("--archived", type=Path, default=ROOT / "_private/original/Hollow Knight")
    parser.add_argument("--report", type=Path, default=ROOT / "build/host/crawler-fsm-source-recheck.json")
    args = parser.parse_args()
    result = verify(args.provenance, args.steam, args.archived)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + "\n")
    print("Build", result["build"], "matched", len(result["files"]), "Steam/archive/probe inputs")
