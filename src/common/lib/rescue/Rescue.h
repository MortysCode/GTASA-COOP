#pragma once
#include <cstdint>
#include "common/lib/buffer/Buffer.h"

namespace coop::rescue {
constexpr uint64_t kBleedMs = 60000, kHelpMs = 3000;
enum Stage : uint8_t { Normal, Downed, Revived, Expired };
struct State {
    uint64_t episode = 0;
    uint8_t stage = Normal;
    uint16_t seconds = 0;
    uint8_t helper = 255;
    bool Active() const { return stage == Downed; }
    void Write(Writer& w) const { w.U64(episode); w.U8(stage); w.U16(seconds); w.U8(helper); }
    bool Read(Reader& r) {
        State s; s.episode = r.U64(); s.stage = r.U8(); s.seconds = r.U16(); s.helper = r.U8();
        if (!r.Ok() || r.Remaining() || s.stage > Expired || (s.helper != 255 && s.helper >= 4)) return false;
        if (s.stage == Normal ? (s.episode || s.seconds || s.helper != 255) : !s.episode) return false;
        if (s.Active() ? (!s.seconds || s.seconds > 60) : (s.seconds || s.helper != 255)) return false;
        *this = s; return true;
    }
};
class Cycle {
public:
    void Begin(uint64_t episode, uint64_t now) { m_state = {episode, Downed, 60, 255}; m_deadline = now + kBleedMs; m_helpAt = 0; }
    bool Help(uint8_t who, uint64_t now) {
        if (!m_state.Active() || now >= m_deadline || who >= 4 || m_state.helper != 255) return false;
        m_state.helper = who; m_helpAt = now + kHelpMs; return true;
    }
    void CancelHelp() { m_state.helper = 255; m_helpAt = 0; }
    State Advance(uint64_t now, bool helperValid) {
        if (!m_state.Active()) return m_state;
        if (now >= m_deadline) { m_state.stage = Expired; m_state.seconds = 0; CancelHelp(); }
        else if (m_state.helper != 255 && !helperValid) CancelHelp();
        else if (m_helpAt && now >= m_helpAt) { m_state.stage = Revived; m_state.seconds = 0; CancelHelp(); }
        if (m_state.Active()) m_state.seconds = static_cast<uint16_t>((m_deadline - now + 999) / 1000);
        return m_state;
    }
    State Get() const { return m_state; }
    void Clear() { m_state = {}; m_deadline = m_helpAt = 0; }
private:
    State m_state;
    uint64_t m_deadline = 0, m_helpAt = 0;
};
}  // namespace coop::rescue
