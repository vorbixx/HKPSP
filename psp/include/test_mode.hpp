#pragma once
namespace hkpsp {
// test-mode.cfg third token "restore=save": restore tutorial.sav in capture mode, so a staged save can
// start a capture in its room (original | PSP comparisons; user 2026-11-18 "compare before merging").
// Shipped play (no test-mode.cfg) is unaffected.
inline bool captureRestoresSave=false;
}
