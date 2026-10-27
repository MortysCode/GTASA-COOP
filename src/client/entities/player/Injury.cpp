#include <map>
#include "entities/player/Injury.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"

namespace coop {
namespace {
std::map<uint8_t, rescue::State> g_injuries;
LethalHandler g_lethal = nullptr;
}
rescue::State PlayerInjury(uint8_t id) {
    auto it = g_injuries.find(id);
    return it == g_injuries.end() ? rescue::State{} : it->second;
}
void SetPlayerInjury(uint8_t id, rescue::State state) { g_injuries[id] = state; }
void ClearPlayerInjury(uint8_t id) { g_injuries.erase(id); }
void ClearAllPlayerInjuries() { g_injuries.clear(); }
bool InjuryProtects(game::CPed* ped) {
    if (!ped || !Net().Online()) return false;
    auto* remote = Players::Instance().FindByPed(ped);
    return PlayerInjury(remote ? remote->id : ped == game::FindPlayerPed(0) ? Net().MyId() : 255).Active();
}
void SetLethalHandler(LethalHandler handler) { g_lethal = handler; }
bool InterceptLethalDamage(game::CPed* ped, int weapon, float healthBefore) {
    return g_lethal && g_lethal(ped, weapon, healthBefore);
}
}  // namespace coop
