#pragma once
// Version of the converted data the EBOOT reads. tools/psp/pack_text.py writes
// it to `data/format.txt`; at boot a different value stops the game with
// "rebuild the data" instead of reading mismatched files. A release that
// needs new or changed data (any converter output or text key) raises it, so
// players of an EBOOT-only update keep their data until it is incompatible.
// tests/psp/test_text_pack.py ties the text keys to `textKeysHash`.
namespace hkpsp {
inline constexpr unsigned dataFormat=1;
inline constexpr const char* textKeysHash="3dce545024a0cea1";
}
