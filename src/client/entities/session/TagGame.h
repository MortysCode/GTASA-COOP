#pragma once
#include <cstdint>
namespace game {
struct CPed;
struct CEntity;
} // namespace game
namespace coop {
bool TagGameActive();
bool TagGameHidden(uint8_t id);
bool TagGameBlind();
bool TagGameProtects(game::CPed *victim);
bool TagGameHit(game::CPed *victim, game::CEntity *attacker, int weapon);
void DrawTagGameOverlay(); // final overlay, including on pause/menu screens
} // namespace coop
