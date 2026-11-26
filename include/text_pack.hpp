#pragma once
// Original game text, loaded at run time from the converted text pack.
//
// Source files and the EBOOT carry no original text: tables hold keys such as
// "@Cornifer/CORNIFER_MEET" and consumers resolve them here. The pack
// (`text.bin`) is built from the player's Steam files by tools/psp/pack_text.py.
#include <cstdint>
#include <vector>

namespace hk3ds::text {

inline constexpr char keyPrefix='@';

// Replaces the loaded pack. Returns false (and keeps nothing) when malformed.
bool load(std::vector<char> bytes);
bool loaded();
std::uint32_t count();
// Text for a key without the prefix, or nullptr.
const char* find(const char* key);
// Keys ("@...") resolve to their text; a missing key returns the key itself so
// the gap is visible. Any other string (and nullptr) is returned unchanged.
const char* resolve(const char* text);
bool isKey(const char* text);
// Keys resolve() could not find since the last load.
std::uint32_t missing();

}
