#pragma once
#include "common/lib/afk/Afk.h"

namespace game { struct CPed; }
namespace coop {

afk::State PlayerAfk(uint8_t id);
void SetPlayerAfk(uint8_t id, afk::State state);
void ClearPlayerAfk(uint8_t id);
void ClearAllPlayerAfk();
bool AfkProtects(game::CPed* ped);
const char* AfkPhrase(uint8_t index);

}  // namespace coop
