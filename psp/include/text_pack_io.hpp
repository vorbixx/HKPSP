#pragma once
#include <string>

namespace hkpsp {

class Platform;

// Checks `format.txt` against the EBOOT's data format, then loads `text.bin`.
// Throws std::runtime_error with a "rebuild the data" message on a mismatch.
// Loads once; later calls return immediately.
void loadGameText(const Platform& platform);
// Text of the data-format check, for tests: an empty string when it passes.
std::string dataFormatError(const std::string& formatFile);

}
