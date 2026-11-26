#!/usr/bin/env bash
# Builds a PSP target with the pspdev toolchain in WSL, isolated from the Windows PATH.
#   wsl -d Ubuntu-24.04 -- bash /mnt/c/Projects/HKPSP/tools/psp/wsl-make.sh <dir relative to repo> [make args]
# Toolchain: ~/pspdev (pspdev v20261101 ubuntu x86_64, docs/psp/toolchain.md).
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
dir="${1:?usage: wsl-make.sh <dir> [make args]}"; shift
export PSPDEV="$HOME/pspdev"
export PATH="$PSPDEV/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
grep -q 'export PSPDEV=' "$HOME/.bashrc" 2>/dev/null || \
  printf '\nexport PSPDEV="$HOME/pspdev"\nexport PATH="$PATH:$PSPDEV/bin"\n' >> "$HOME/.bashrc"
exec make -C "$repo/$dir" -j"$(nproc)" "$@"
