#pragma once
#include "common/lib/afk/Afk.h"
#include "common/lib/rescue/Rescue.h"

namespace game { struct CPed; }
namespace coop {
rescue::State PlayerInjury(uint8_t id);
void SetPlayerInjury(uint8_t id, rescue::State state);
void ClearPlayerInjury(uint8_t id);
void ClearAllPlayerInjuries();
bool InjuryProtects(game::CPed* ped);
using LethalHandler = bool (*)(game::CPed*, int weapon, float healthBefore);
void SetLethalHandler(LethalHandler handler);
bool InterceptLethalDamage(game::CPed* ped, int weapon, float healthBefore);
}  // namespace coop
