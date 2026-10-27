#include <iostream>
#include <stdexcept>
#include "common/lib/afk/Afk.h"

using namespace coop;
static void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

int main() {
    try {
        for (uint8_t i = 0; i < afk::kPhraseCount; ++i) {
            Writer w; afk::State{true, i}.Write(w);
            Reader r(w.Data(), w.Size()); afk::State got;
            Require(got.Read(r) && got.active && got.phrase == i, "phrase round trip");
        }
        // A malformed packet must not replace an already active status.
        for (const auto& data : {std::vector<uint8_t>{}, {1}, {2, 0}, {1, 8}, {0, 3}, {1, 0, 0}}) {
            Reader r(data.data(), data.size()); afk::State got{true, 2};
            Require(!got.Read(r) && got.active && got.phrase == 2, "malformed packet replaced status");
        }
        afk::States states;
        Require(!states.Get(0).active && !states.Get(1).active, "initial damage protection");
        states.Set(0, {true, 1}); states.Set(1, {true, 4});
        Require(states.Get(0).active && states.Get(1).phrase == 4, "independent peers");
        states.Set(0, {});
        Require(!states.Get(0).active && states.Get(1).active, "toggle off leaves other peer alone");
        states.Set(0, {true, 3});
        states.Remove(0);
        Require(!states.Get(0).active, "reused player slot inherited AFK protection");
        states.Clear();
        Require(!states.Get(0).active && !states.Get(1).active, "disconnect cleanup");
        // Every lethal / armour damage field must be suppressed, including an
        // already prepared forced-death response, while retaining calculated=1.
        afk::DamageResponse damage{1000, 250, 1, 1, 0, 1};
        damage.Suppress();
        Require(damage.health == 0 && damage.armour == 0 && !damage.healthZero && !damage.forceDeath &&
                damage.calculated == 1 && !damage.checkAffectsPed, "AFK response still inflicts damage");
        std::cout << "AFK state, malformed packet, cleanup and damage-response tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
