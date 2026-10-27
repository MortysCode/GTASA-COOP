#include "entities/session/TagGame.h"
#include <windows.h>
#include <algorithm>
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Afk.h"
#include "entities/player/ControlLocks.h"
#include "entities/player/Injury.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {
namespace {
using namespace game;
using game::script::Cmd;
constexpr uint16_t kAidControlBit = controls::kAid;
constexpr const char* kDownAnim = "KO_shot_stom";

class Revive : public Module {
public:
    const char* Name() const override { return "Revive"; }
    void OnInstallHooks() override {
        FILETIME stamp; GetSystemTimeAsFileTime(&stamp);
        m_nextEpisode = (uint64_t(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime;
        SetLethalHandler([](CPed* ped, int weapon, float hp) { return Instance().Wound(ped, weapon, hp); });
        Net().SetHandler(proto::G_PLAYER_INJURY, [](uint8_t from, Reader& r) {
            rescue::State state;
            if (state.Read(r) && (state.helper == 255 || state.helper != from)) SetPlayerInjury(from, state);
        });
        Net().SetHandler(proto::G_REVIVE_ACTION, [](uint8_t from, Reader& r) { Instance().Action(from, r); });
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd == "giveup") {
            if (args.empty() && m_cycle.Get().Active()) Reset(true);
            else ui::TheChat().AddError(TR("Команда /giveup без аргументов позволяет сдаться, пока вы тяжело ранены.",
                                          "Use /giveup without arguments to give up while badly wounded."));
            return true;
        }
        if (cmd != "revive") return false;
        if (!args.empty()) {
            ui::TheChat().AddError(TR("Используйте /revive без аргументов рядом с раненым напарником.",
                                     "Use /revive without arguments next to your wounded partner."));
            return true;
        }
        BeginHelp(true);
        return true;
    }
    bool OnKeyDown(int vk) override {
        if (vk != Cfg().reviveKey) return false;
        if (m_requestedTarget != 255) return true;
        const auto* target = InteractionTarget();
        if (!target) return false;  // ordinary game input remains available away from a wounded partner
        m_requestedTarget = target->id;
        m_requestedEpisode = PlayerInjury(target->id).episode;
        m_requestedAt = NowMs();
        return true;
    }
    void OnPreProcess() override {
        if (m_requestedTarget != 255) {
            auto target = m_requestedTarget; auto episode = m_requestedEpisode; auto at = m_requestedAt;
            ClearRequest();
            // Recheck on the simulation frame: a key from a closed chat/menu or
            // an older wound must never start a new rescue later.
            if (NowMs() - at <= 500) BeginHelp(false, target, episode);
        }
        Tick();
    }
    void OnPostProcess() override { Tick(); }
    void OnPeerJoin(uint8_t) override { SendState(); }
    void OnPeerResync(uint8_t id) override {
        if (m_requestedTarget == id) ClearRequest();
        ClearPlayerInjury(id);
        if (m_helpTarget == id) StopHelping(false);
        if (m_cycle.Get().helper == id) { m_cycle.CancelHelp(); Publish(); }
        SendState();
    }
    void OnPeerLeave(uint8_t id) override {
        if (m_requestedTarget == id) ClearRequest();
        ClearPlayerInjury(id);
        if (m_helpTarget == id) StopHelping(false);
        if (m_cycle.Get().helper == id) { m_cycle.CancelHelp(); Publish(); }
    }
    void OnSessionStart() override { Reset(false); ClearAllPlayerInjuries(); SendState(); }
    void OnSessionEnd() override { Reset(true); ClearAllPlayerInjuries(); }
    void OnGameShutdown() override { Reset(false); }
    void OnRender() override {
        auto state = m_cycle.Get();
        bool helping = m_helpTarget != 255;
        bool offer = !state.Active() && !helping && InteractionTarget();
        if (!state.Active() && !helping && !offer) return;
        auto& g = ui::Gfx(); float s = g.Scale();
        std::string text = state.Active()
            ? Format(TR("ВЫ РАНЕНЫ: %d сек. Напарник может подойти и помочь.",
                        "YOU ARE WOUNDED: %d sec. Your partner can approach and help."), state.seconds)
            : helping ? TR("ОКАЗЫВАЕТЕ ПОМОЩЬ — оставайтесь на месте", "HELPING YOUR PARTNER — stay still")
                      : Format(TR("[%s] Поднять напарника", "[%s] Revive your partner"), KeyLabel().c_str());
        float width = std::min(720.0f * s, g.Width() - 32 * s);
        auto lines = g.Wrap(ui::FONT_NORMAL, Utf8ToWide(text), width - 32 * s);
        float lh = g.LineHeight(ui::FONT_NORMAL);
        float height = 24 * s + lh * lines.size() + (offer ? g.LineHeight(ui::FONT_SMALL) + 8 * s : helping ? 18 * s : 0);
        float x = (g.Width() - width) / 2, y = g.Height() - 90 * s - height;
        g.Rect(x, y, width, height, 0xD0101820);
        g.RectOutline(x, y, width, height, state.Active() ? 0xFFFF6655 : 0xFF80D080);
        float ty = y + 12 * s;
        for (const auto& line : lines) {
            g.TextW(ui::FONT_NORMAL, g.Width() / 2, ty, line, 0xFFFFDD88, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
            ty += lh;
        }
        if (offer) g.Text(ui::FONT_SMALL, g.Width() / 2, ty + 4 * s,
                         TR("Одно нажатие, затем 3 секунды рядом", "Press once, then stay close for 3 seconds"),
                         0xFFE0E8E0, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        if (helping) {
            float progress = m_helpAccepted ? std::clamp(float(NowMs() - m_helpProgressAt) / float(rescue::kHelpMs), 0.0f, 1.0f) : 0.0f;
            g.Rect(x + 16 * s, ty + 6 * s, width - 32 * s, 6 * s, 0xFF364036);
            g.Rect(x + 16 * s, ty + 6 * s, (width - 32 * s) * progress, 6 * s, 0xFF80DD88);
        }
    }
    static Revive& Instance() { static Revive r; return r; }

private:
    rescue::Cycle m_cycle;
    uint64_t m_nextEpisode = 0, m_lastSend = 0, m_lastPose = 0;
    int m_ped = -1;
    bool m_locked = false, m_poseStarted = false;
    uint8_t m_helpTarget = 255;
    uint64_t m_helpEpisode = 0, m_helpSince = 0;
    bool m_helpAccepted = false;
    uint64_t m_helpProgressAt = 0;
    uint8_t m_requestedTarget = 255;
    uint64_t m_requestedEpisode = 0, m_requestedAt = 0;
    float m_helperHP = 0, m_helperArmour = 0;
    CVector m_helperPos;
    uint8_t m_rescuerHP = 0, m_rescuerArmour = 0;

    void ClearRequest() { m_requestedTarget = 255; m_requestedEpisode = m_requestedAt = 0; }
    static bool UiAllowsAction() {
        return game::IsForeground() && !game::MenuActive() && !core::ConnectMenuVisible() &&
               !ui::TheChat().IsOpen() && !ui::In().KeyboardCaptured() && !ui::In().MouseCaptured();
    }
    static std::string KeyLabel() {
        int key = Cfg().reviveKey;
        if (key >= 'A' && key <= 'Z') return std::string(1, static_cast<char>(key));
        wchar_t label[64]{};
        LONG scan = static_cast<LONG>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16);
        if (GetKeyNameTextW(scan, label, 64)) return WideToUtf8(label);
        return TR("Помощь", "Help");
    }
    const RemotePlayer* NearbyWounded(bool unclaimed) const {
        const RemotePlayer* target = nullptr;
        float best = 3.01f;
        for (const auto& [id, remote] : Players::Instance().All()) {
            auto state = PlayerInjury(id);
            if (!state.Active() || (unclaimed && state.helper != 255) || !Close(remote, 2000)) continue;
            float distance = (remote.state.pos - FindPlayerPed(0)->GetPosition()).Length();
            if (distance < best) { best = distance; target = &remote; }
        }
        return target;
    }
    const RemotePlayer* InteractionTarget() const {
        if (m_helpTarget != 255 || m_requestedTarget != 255 || !UiAllowsAction() || !LocalFree() ||
            ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU)) & 0x8000)) return nullptr;
        return NearbyWounded(true);
    }
    bool BeginHelp(bool feedback, uint8_t preferred = 255, uint64_t episode = 0) {
        auto error = [&](const char* text) { if (feedback) ui::TheChat().AddError(text); return false; };
        if (m_helpTarget != 255) return error(TR("Вы уже помогаете напарнику. Оставайтесь рядом.", "You are already helping. Stay nearby."));
        if (!UiAllowsAction() || !LocalFree()) return error(TR("Для помощи выйдите из транспорта и завершите AFK или другие действия.",
                                                               "To help, leave your vehicle and finish AFK or other activities."));
        const RemotePlayer* target = preferred == 255 ? NearbyWounded(false) : Players::Instance().Get(preferred);
        if (!target || !Close(*target, 2000)) return error(TR("Подойдите к раненому напарнику на расстояние до 3 метров.",
                                                            "Stand within 3 metres of your wounded partner."));
        auto state = PlayerInjury(target->id);
        if (!state.Active() || (episode && state.episode != episode)) return false;
        if (state.helper != 255) return error(TR("Напарнику уже оказывают помощь.", "Someone is already helping your partner."));
        m_helpTarget = target->id; m_helpEpisode = state.episode; m_helpSince = NowMs(); m_helpAccepted = false; m_helpProgressAt = 0;
        m_helperHP = FindPlayerPed(0)->Health(); m_helperArmour = FindPlayerPed(0)->Armour();
        m_helperPos = FindPlayerPed(0)->GetPosition();
        Lock(); SendAction(m_helpTarget, m_helpEpisode, 0);
        ui::TheChat().AddSystem(TR("Оказываете помощь. Оставайтесь рядом 3 секунды; получение урона прервёт спасение.",
                                  "Helping your partner. Stay nearby for 3 seconds; taking damage interrupts the rescue."));
        return true;
    }

    bool LocalFree() const {
        if (TagGameActive()) return false;
        if (!Net().Online() || !flow::InSharedWorld() || !core::IsPlayerReady() || core::IsRestarting()) return false;
        auto* me = FindPlayerPed(0);
        return me->Health() > 0 && PlayerInfos()[0].playerState == 0 && !me->InVehicle() && !me->IsInTheAir() &&
               !GetPad(0)->DisablePlayerControls && !cam::WideScreenOn() && !Ref<uint8_t>(0xB5F851) &&
               !PlayerAfk(Net().MyId()).active && !PlayerInjury(Net().MyId()).Active() && ActivePrimaryTaskType(me) == 0;
    }
    bool Close(const RemotePlayer& remote, uint64_t fresh) const {
        auto* me = FindPlayerPed(0);
        return me && remote.ped && remote.hasState && NowMs() - remote.stateRecvMs < fresh &&
               remote.mode == RemotePlayer::Mode::OnFoot && remote.state.area == me->AreaCode() &&
               !(remote.state.flags & OnFootState::F_DEAD) && remote.state.health > 0 &&
               (remote.state.pos - me->GetPosition()).Length() <= 3.0f;
    }
    bool Wound(CPed* ped, int weapon, float hpBefore) {
        if (TagGameActive()) return false;
        if (!Net().Online() || !flow::InSharedWorld() || !core::IsPlayerReady() || core::IsRestarting() || hpBefore <= 0 ||
            ped != FindPlayerPed(0) || Players::Instance().FindByPed(ped) || m_cycle.Get().Active() ||
            ped->InVehicle() || ped->IsInTheAir() || !(ped->PedFlags() & 1) || weapon == 53 /* drowning */ || cam::WideScreenOn() ||
            Ref<uint8_t>(0xB5F851) || (GetPad(0)->DisablePlayerControls & ~kAidControlBit) || PlayerAfk(Net().MyId()).active)
            return false;
        bool canBeHelped = false;
        for (const auto& [id, remote] : Players::Instance().All())
            if (remote.hasState && NowMs() - remote.stateRecvMs < 2000 && remote.state.health > 0 &&
                !(remote.state.flags & OnFootState::F_DEAD) && !PlayerInjury(id).Active() && !PlayerAfk(id).active)
                canBeHelped = true;
        if (!canBeHelped) return false;
        StopHelping(true);
        m_cycle.Begin(++m_nextEpisode, NowMs());
        m_ped = GetPedRef(ped); m_poseStarted = false;
        ped->Health() = 1.0f; ped->Armour() = 0.0f;
        Lock(); Publish();
        ui::TheChat().AddSystem(TR("Вы тяжело ранены. У напарника есть 60 секунд: подойти и нажать кнопку помощи.",
                                  "You are badly wounded. Your partner has 60 seconds to approach and press the help key."));
        LOGI("Wounded: episode %llu, weapon %d", (unsigned long long)m_cycle.Get().episode, weapon);
        return true;
    }
    void Lock() { AcquirePlayerControlLock(kAidControlBit); m_locked = true; }
    void Unlock() { if (m_locked) ReleasePlayerControlLock(kAidControlBit); m_locked = false; }
    void ClearPose() {
        if (m_poseStarted && core::IsPlayerReady() && !core::IsRestarting() && GetPed(m_ped) == FindPlayerPed(0) &&
            Cmd(0x0611).I(m_ped).S(kDownAnim).Run()) Cmd(0x0687).I(m_ped).Run();
        m_poseStarted = false;
    }
    void SendAction(uint8_t target, uint64_t episode, uint8_t op) {
        if (!Net().Online()) return;
        Writer w(proto::G_REVIVE_ACTION); w.U8(target); w.U64(episode); w.U8(op); Net().Send(proto::CH_RELIABLE, w);
    }
    void StopHelping(bool cancel) {
        if (m_helpTarget == 255) return;
        if (cancel) SendAction(m_helpTarget, m_helpEpisode, 1);
        m_helpTarget = 255; m_helpEpisode = 0; m_helpAccepted = false;
        m_helpProgressAt = 0;
        if (!m_cycle.Get().Active()) Unlock();
    }
    void Action(uint8_t from, Reader& r) {
        uint8_t target = r.U8(); uint64_t episode = r.U64(); uint8_t op = r.U8();
        if (!r.Ok() || r.Remaining() || target != Net().MyId() || !episode || op > 2) return;
        if (op == 2) {
            if (from == m_helpTarget && episode == m_helpEpisode) { StopHelping(false); ui::TheChat().AddError(TR("Помощь не началась: проверьте расстояние и состояние напарника.", "Help could not start: check distance and your partner's condition.")); }
            return;
        }
        auto state = m_cycle.Get();
        if (!state.Active() || state.episode != episode) { if (!op) SendAction(from, episode, 2); return; }
        if (op == 1) {
            if (state.helper == from) { m_cycle.CancelHelp(); Publish(); }
            return;
        }
        auto* helper = Players::Instance().Get(from);
        if (!core::IsPlayerReady() || !helper || !Close(*helper, 1500) || PlayerAfk(from).active || PlayerInjury(from).Active() ||
            !m_cycle.Help(from, NowMs())) { SendAction(from, episode, 2); return; }
        m_rescuerHP = helper->state.health; m_rescuerArmour = helper->state.armour;
        Publish();
    }
    void SendState() {
        if (!Net().Online()) return;
        Writer w(proto::G_PLAYER_INJURY); m_cycle.Get().Write(w); Net().Send(proto::CH_RELIABLE, w); m_lastSend = NowMs();
    }
    void Publish() { SetPlayerInjury(Net().MyId(), m_cycle.Get()); SendState(); }

    void Tick() {
        if (!core::IsPlayerReady() || core::IsRestarting()) return;
        auto* me = FindPlayerPed(0); uint64_t now = NowMs();
        auto before = m_cycle.Get();
        if (before.Active()) {
            if (!Net().Online() || !flow::InSharedWorld() || GetPed(m_ped) != me || me->InVehicle() || me->Health() <= 0 ||
                cam::WideScreenOn() || Ref<uint8_t>(0xB5F851) || (GetPad(0)->DisablePlayerControls & ~kAidControlBit)) {
                Reset(true); return;
            }
            Lock();
            auto* helper = before.helper == 255 ? nullptr : Players::Instance().Get(before.helper);
            bool valid = helper && Close(*helper, 1500) && !PlayerAfk(helper->id).active && !PlayerInjury(helper->id).Active() &&
                         helper->state.health >= m_rescuerHP && helper->state.armour >= m_rescuerArmour;
            auto state = m_cycle.Advance(now, valid);
            SetPlayerInjury(Net().MyId(), state);
            if (!state.Active()) {
                ClearPose(); Unlock();
                me->Health() = state.stage == rescue::Revived ? 30.0f : 0.0f;
                me->Armour() = 0;
                Publish();
                ui::TheChat().AddSystem(state.stage == rescue::Revived
                    ? TR("Напарник поднял вас. Здоровье: 30 HP.", "Your partner revived you. Health: 30 HP.")
                    : TR("Время на помощь истекло.", "Time to receive help ran out."));
            } else {
                if (now - m_lastPose >= 1000) {
                    m_lastPose = now;
                    if (!Cmd(0x0611).I(m_ped).S(kDownAnim).Run()) {
                        Cmd(0x0605).I(m_ped).S(kDownAnim).S("PED").F(8.0f).I(0).I(1).I(1).I(1).I(-1).Run();
                        m_poseStarted = true;
                    }
                }
                if (state.helper != before.helper || now - m_lastSend >= 1000) SendState();
            }
        }
        if (m_helpTarget != 255) {
            auto state = PlayerInjury(m_helpTarget);
            auto* target = Players::Instance().Get(m_helpTarget);
            bool alive = me->Health() > 0 && !me->InVehicle() && !cam::WideScreenOn() && !Ref<uint8_t>(0xB5F851);
            bool safe = alive && target && Close(*target, 1500) && !(GetPad(0)->DisablePlayerControls & ~kAidControlBit) &&
                        me->Health() >= m_helperHP && me->Armour() >= m_helperArmour &&
                        (me->GetPosition() - m_helperPos).Length() < 0.5f;
            if (state.episode == m_helpEpisode && state.stage == rescue::Revived) {
                StopHelping(false); ui::TheChat().AddSystem(TR("Вы подняли напарника.", "You revived your partner."));
            } else if (!safe || !state.Active() || state.episode != m_helpEpisode || now - m_helpSince > 8000 ||
                       (m_helpAccepted && state.helper != Net().MyId())) {
                StopHelping(true); ui::TheChat().AddError(TR("Помощь прервана.", "Rescue interrupted."));
            } else if (state.helper == Net().MyId()) {
                if (!m_helpAccepted) m_helpProgressAt = now;
                m_helpAccepted = true; Lock();
            }
            else if (now - m_helpSince > 2500) { StopHelping(true); ui::TheChat().AddError(TR("Напарник не подтвердил помощь.", "Your partner did not confirm the rescue.")); }
        }
    }
    void Reset(bool kill) {
        ClearRequest();
        bool down = m_cycle.Get().Active();
        StopHelping(true); ClearPose(); Unlock(); m_cycle.Clear();
        if (kill && down && core::IsPlayerReady() && !core::IsRestarting()) FindPlayerPed(0)->Health() = 0;
        m_ped = -1; Publish();
    }
};
struct Reg { Reg() { core::RegisterModule(&Revive::Instance()); } } g_reg;
}  // namespace
}  // namespace coop
