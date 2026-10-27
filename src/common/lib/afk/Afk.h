#pragma once
#include <cstdint>
#include <cstddef>
#include <map>

#include "common/lib/buffer/Buffer.h"

namespace coop::afk {

constexpr uint8_t kPhraseCount = 8;

struct State {
    bool active = false;
    uint8_t phrase = 0;

    void Write(Writer& w) const { w.Bool(active); w.U8(active ? phrase : 0); }
    bool Read(Reader& r) {
        uint8_t on = r.U8(), text = r.U8();
        if (!r.Ok() || r.Remaining() || on > 1 || text >= kPhraseCount || (!on && text)) return false;
        active = on != 0;
        phrase = text;
        return true;
    }
};

class States {
public:
    State Get(uint8_t id) const {
        auto it = m_states.find(id);
        return it == m_states.end() ? State{} : it->second;
    }
    void Set(uint8_t id, State state) {
        if (state.active && state.phrase < kPhraseCount) m_states[id] = state;
        else m_states.erase(id);
    }
    void Remove(uint8_t id) { m_states.erase(id); }
    void Clear() { m_states.clear(); }

private:
    std::map<uint8_t, State> m_states;
};

// Layout of SA 1.0 US CPedDamageResponse (plugin-sdk). No proof flags are set
// on the ped, so saving/loading a world cannot persist AFK invulnerability.
struct DamageResponse {
    float health = 0, armour = 0;
    uint8_t healthZero = 0, forceDeath = 0, calculated = 0, checkAffectsPed = 0;
    void Suppress() { *this = {}; calculated = 1; }
};
static_assert(sizeof(DamageResponse) == 0xC);
static_assert(offsetof(DamageResponse, calculated) == 0xA);

}  // namespace coop::afk
