#include "entities/session/TagGame.h"
#include <random>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Afk.h"
#include "entities/player/ControlLocks.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop {
namespace {
using namespace game;
using game::script::Cmd;

constexpr const char* kBlock = "SMOKING";
constexpr const char* kAnimation = "M_smkstnd_loop";
// Unused bit in SA's CPad::DisablePlayerControls (plugin-sdk). Unlike the
// SET_PLAYER_CONTROL bit (0x20), it doesn't announce a mission cutscene.
constexpr uint16_t kAfkControlBit = controls::kAfk;

class Afk : public Module {
public:
    const char* Name() const override { return "Afk"; }
    void OnInstallHooks() override {
        Net().SetHandler(proto::G_PLAYER_AFK, [](uint8_t from, Reader& r) {
            afk::State state;
            if (state.Read(r)) SetPlayerAfk(from, state);
        });
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd != "afk") return false;
        auto& chat = ui::TheChat();
        if (!args.empty()) {
            chat.AddError(TR("Используйте /afk без аргументов: включить или выключить AFK.",
                             "Use /afk without arguments to toggle AFK."));
            return true;
        }
        if (TagGameActive()) return true;
        if (m_active) { Stop(true); return true; }
        if (!Net().Online() || !flow::InSharedWorld() || !core::IsPlayerReady()) {
            chat.AddError(TR("AFK доступен после подключения и загрузки общего мира.",
                             "AFK is available after joining and loading the shared world."));
            return true;
        }
        CPlayerPed* me = FindPlayerPed(0);
        if (me->Health() <= 0 || me->InVehicle() || me->IsInTheAir() || GetPad(0)->DisablePlayerControls ||
            cam::WideScreenOn() || Ref<uint8_t>(0xB5F851) || ActivePrimaryTaskType(me) != 0) {
            chat.AddError(TR("Для AFK встаньте на землю, выйдите из транспорта и завершите текущее действие.",
                             "To go AFK, stand on the ground, leave your vehicle and finish the current activity."));
            return true;
        }
        uint8_t phrase = static_cast<uint8_t>(m_rng() % (m_lastPhrase < afk::kPhraseCount ? afk::kPhraseCount - 1 : afk::kPhraseCount));
        if (m_lastPhrase < afk::kPhraseCount && phrase >= m_lastPhrase) ++phrase;
        m_lastPhrase = phrase;
        m_active = true;
        m_ped = GetPedRef(me);
        m_started = NowMs();
        SetPlayerAfk(Net().MyId(), {true, phrase});
        AcquirePlayerControlLock(kAfkControlBit);
        m_inputHeld = true;
        Cmd(0x04ED).S(kBlock).Run();  // REQUEST_ANIMATION, asynchronous
        m_requested = true;
        SendState();
        chat.AddSystem(Format(TR("AFK: << %s >>. Повторите /afk, чтобы вернуться.",
                                 "AFK: << %s >>. Type /afk again to return."), AfkPhrase(phrase)));
        LOGI("AFK enabled, phrase %u", phrase);
        return true;
    }

    void OnPreProcess() override {
        if (!m_active) return;
        if (!CanRemain()) { Stop(true); return; }
        AcquirePlayerControlLock(kAfkControlBit);
        if (NowMs() - m_lastAnimCheck < 500) return;
        m_lastAnimCheck = NowMs();
        if (Cmd(0x04EE).S(kBlock).Run()) {  // HAS_ANIMATION_LOADED
            if (!Cmd(0x0611).I(m_ped).S(kAnimation).Run())
                Cmd(0x0605).I(m_ped).S(kAnimation).S(kBlock).F(4.0f).I(1).I(0).I(0).I(0).I(-1).Run();
        } else if (NowMs() - m_started > 8000) {
            Stop(false);
            ui::TheChat().AddError(TR("Не удалось загрузить анимацию курения. AFK отключён.",
                                      "Could not load the smoking animation. AFK was disabled."));
        }
    }
    void OnPostProcess() override { if (m_active && !CanRemain()) Stop(true); }
    void OnPeerJoin(uint8_t) override { SendState(); }
    void OnPeerResync(uint8_t id) override { ClearPlayerAfk(id); SendState(); }
    void OnPeerLeave(uint8_t id) override { ClearPlayerAfk(id); }
    void OnGameShutdown() override { Stop(false); }
    void OnSessionEnd() override { Stop(false); ClearAllPlayerAfk(); }
    void OnSessionStart() override { Stop(false); ClearAllPlayerAfk(); SendState(); }

private:
    bool m_active = false, m_requested = false, m_inputHeld = false;
    int m_ped = -1;
    uint8_t m_lastPhrase = afk::kPhraseCount;
    uint64_t m_started = 0, m_lastAnimCheck = 0;
    std::mt19937 m_rng{static_cast<uint32_t>(NowMs())};

    bool CanRemain() const {
        if (!Net().Online() || !flow::InSharedWorld() || !core::IsPlayerReady() || core::IsRestarting()) return false;
        CPlayerPed* me = FindPlayerPed(0);
        return GetPed(m_ped) == me && me->Health() > 0 && !me->InVehicle() &&
               !(GetPad(0)->DisablePlayerControls & ~kAfkControlBit) && !cam::WideScreenOn() && !Ref<uint8_t>(0xB5F851);
    }
    void SendState() {
        if (!Net().Online()) return;
        Writer w(proto::G_PLAYER_AFK);
        PlayerAfk(Net().MyId()).Write(w);
        Net().Send(proto::CH_RELIABLE, w);
    }
    void Stop(bool notify) {
        bool wasActive = m_active;
        m_active = false;
        ClearPlayerAfk(Net().MyId());  // drop protection before restoring actions
        if (m_inputHeld) ReleasePlayerControlLock(kAfkControlBit);
        m_inputHeld = false;
        if (core::IsPlayerReady() && !core::IsRestarting() && GetPed(m_ped) == FindPlayerPed(0) &&
            Cmd(0x0611).I(m_ped).S(kAnimation).Run())
            Cmd(0x0687).I(m_ped).Run();  // CLEAR_CHAR_TASKS, only if our own named animation still plays
        if (m_requested) Cmd(0x04EF).S(kBlock).Run();  // REMOVE_ANIMATION: release only our request
        m_requested = false;
        m_ped = -1;
        if (wasActive) {
            SendState();
            LOGI("AFK disabled");
            if (notify) ui::TheChat().AddSystem(TR("AFK отключён. Вы вернулись в игру.", "AFK disabled. You are back in the game."));
        }
    }
};

Afk g_afk;
struct Reg { Reg() { core::RegisterModule(&g_afk); } } g_reg;

}  // namespace
}  // namespace coop
