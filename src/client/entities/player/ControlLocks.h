#pragma once
#include "common/lib/control-locks/ControlLocks.h"

namespace coop {
void AcquirePlayerControlLock(uint16_t owner);
void ReleasePlayerControlLock(uint16_t owner);
}  // namespace coop
