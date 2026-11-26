#pragma once

namespace hk3ds {
// Original PersistentBoolItem (Ruins2_03, Battle Control).Activated and the
// separate PlayerData.watcherChandelier value. Neither implies the other.
struct WatcherProgress {bool battleActivated=false,watcherChandelier=false;};
}
