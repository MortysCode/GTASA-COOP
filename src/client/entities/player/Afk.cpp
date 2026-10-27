#include "entities/player/Afk.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"

namespace coop {
namespace {
afk::States g_afk;
}

afk::State PlayerAfk(uint8_t id) { return g_afk.Get(id); }
void SetPlayerAfk(uint8_t id, afk::State state) { g_afk.Set(id, state); }
void ClearPlayerAfk(uint8_t id) { g_afk.Remove(id); }
void ClearAllPlayerAfk() { g_afk.Clear(); }

bool AfkProtects(game::CPed* ped) {
    if (!ped || !Net().Online()) return false;
    if (ped == game::FindPlayerPed(0)) return PlayerAfk(Net().MyId()).active;
    auto* remote = Players::Instance().FindByPed(ped);
    return remote && PlayerAfk(remote->id).active;
}

const char* AfkPhrase(uint8_t index) {
    switch (index) {
        case 0: return TR("Ушёл поссать", "Gone for a piss");
        case 1: return TR("Дрочу", "Having a wank");
        case 2: return TR("Минутка на курение", "Smoke break");
        case 3: return TR("Отошёл за пивом", "Getting a beer");
        case 4: return TR("Залип в телефоне", "Lost in my phone");
        case 5: return TR("Потерялся на кухне", "Lost in the kitchen");
        case 6: return TR("Ща вернусь", "Be right back");
        default: return TR("Перекур у Си-Джея", "CJ's smoke break");
    }
}

}  // namespace coop
