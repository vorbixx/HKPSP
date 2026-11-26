#pragma once

// Host tests and direct make builds retain the diagnostic interfaces.
#ifndef HK3DS_DIAGNOSTICS
#define HK3DS_DIAGNOSTICS 1
#endif
namespace hk3ds {
inline constexpr bool diagnosticsEnabled=HK3DS_DIAGNOSTICS!=0;
}
