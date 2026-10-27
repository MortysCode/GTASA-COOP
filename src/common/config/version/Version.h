#pragma once

#define COOP_NAME "SA:COOP"
#define COOP_VERSION_STR "0.3.1.9"  // launcher installation from an owned GTA copy; gameplay and protocol unchanged

#ifndef RC_INVOKED  // the resource compiler reads only the macros above (VersionInfo.rc)
#include <cstdint>

namespace coop {
// Bumped on any incompatible change of the network protocol.
constexpr uint32_t kProtocolVersion = 25;
constexpr uint16_t kDefaultPort = 7788;
// Size of gta_sa.exe 1.0 US (HOODLUM) — the only supported executable.
constexpr uint32_t kSupportedExeSize = 14383616;
}  // namespace coop
#endif
