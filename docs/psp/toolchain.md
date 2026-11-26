# PSP toolchain (installed 2026-11-15)

- WSL2: distro `Ubuntu-24.04` (user `psp`), installed by the user without a reboot (Virtual Machine
  Platform was already enabled). 12 cores, 15 GB RAM visible.
- Ubuntu packages (root, apt): build-essential cmake pkgconf libreadline8 libusb-0.1-4 libgpgme11
  libarchive-tools fakeroot curl.
- pspdev v20261101 `pspdev-ubuntu-latest-x86_64.tar.gz` (172,966,346 bytes), SHA-256
  5dc9d785a5fd490ee9d5204b90f715efb6c46940ba3944036bbef9c993c17713 = GitHub release digest,
  unpacked to `~/pspdev`; `psp-gcc (GCC) 15.2.0`.
- Build: `wsl -d Ubuntu-24.04 -- bash /mnt/c/Projects/HKPSP/tools/psp/wsl-make.sh <dir> [args]`.
  The wrapper sets PSPDEV and a clean PATH (the Windows PATH leaks into WSL and breaks quoting).
- Smoke test `psp/hello` (C++17, debug screen, sceIo): built and run in PPSSPP 1.20.4 through
  `tools/psp/ppsspp.ps1 -Eboot psp/hello/EBOOT.PBP`; its `ms0:/PSP/GAME/HKPSP/out/hello.txt`
  reached `build/psp/run-*`, and the run stopped on `out/done.txt` after 3.3 s.

Conventions for game builds: self-capture under `ms0:/PSP/GAME/HKPSP/out/`, `done.txt` last.
Sources build from `/mnt/c` (fast enough for now; move to the WSL file system if builds get slow).
Perf EBOOT (HKPSPPERF): default `package.ps1` runs only the shipped CPU config (600 frames, `configs=1`);
`package.ps1 -PerfSweep` (or `make.ps1 PERF=1 PERF_SWEEP=1`) builds the 11-config sweep (`configs=11`, 6600 rows).
