#pragma once

namespace hk3ds {
// Original PlayerData bools. Entering Battle Scene sets encounteredMegaJelly;
// activating the actual corpse sets defeatedMegaJelly, before BOSS DEATH.
// Keep these independent: neither HP loss nor gate opening is a save setter.
struct UumuuProgress {bool encounteredMegaJelly=false,defeatedMegaJelly=false;};
}
