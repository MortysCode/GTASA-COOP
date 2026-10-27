#include "entities/session/TagGame.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "common/lib/tag/Tag.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
#include "entities/mission/Mission.h"
#include "entities/player/Afk.h"
#include "entities/player/ControlLocks.h"
#include "entities/player/Injury.h"
#include "entities/player/Players.h"
#include "entities/profile/Profile.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {
namespace {
using namespace game;
using game::script::Cmd;
constexpr uint16_t kLock = controls::kTag;
constexpr uintptr_t kStaminaStat = 0xB79380 + 22 * sizeof(float);   // CStats::StatTypesFloat[STAT_STAMINA]
constexpr size_t kNetworkStatsSize = 0x4C8, kKeptStatsSize = 0x538; // include sprint/training progress counters
using UpdateFn = void(__cdecl *)();
using SaveFn = bool(__cdecl *)(int);
UpdateFn g_scripts = nullptr, g_pickups = nullptr, g_entries = nullptr;
SaveFn g_save = nullptr;
bool g_hooks = false, g_active = false;
uint32_t g_scriptClock = 0;
tag::State g_state;
uint64_t g_received = 0;

void ShiftScriptWaits() {
    uint32_t now = Ref<uint32_t>(addr::TimeInMs), delta = now - g_scriptClock;
    g_scriptClock = now;
    // CRunningScript offsets from plugin-sdk; Process is paused before it increments script timers.
    void *script = Ref<void *>(0xA8B42C);
    for (int i = 0; script && i < 1000; ++i, script = Field<void *>(script, 0)) {
        auto &wake = Field<uint32_t>(script, 0xCC);
        if (wake)
            wake += delta;
    }
}
void __cdecl Scripts() {
    if (g_active) {
        ShiftScriptWaits();
        return;
    }
    g_scripts();
}
void __cdecl Pickups() {
    if (!g_active)
        g_pickups();
}
void __cdecl Entries() {
    if (!g_active)
        g_entries();
}
bool __cdecl Save(int file) { return !g_active && g_save(file); }

class TagGame : public Module {
  public:
    const char *Name() const override { return "TagGame"; }
    static TagGame &Instance() {
        static TagGame value;
        return value;
    }
    void OnInstallHooks() override {
        bool a = mem::HookFn(0x46A000, &Scripts, &g_scripts, "Tag: pause SCM");
        bool b = mem::HookFn(0x458DE0, &Pickups, &g_pickups, "Tag: disable pickups");
        bool c = mem::HookFn(0x440D10, &Entries, &g_entries, "Tag: disable interiors");
        bool d = mem::HookFn(0x5D13E0, &Save, &g_save, "Tag: block temporary saves");
        g_hooks = a && b && c && d;
        Net().SetHandler(proto::S_TAG, [](uint8_t from, Reader &r) {
            tag::State state;
            if (from == proto::kServerId && state.Read(r))
                Instance().Receive(state);
        });
    }
    bool OnCommand(const std::string &cmd, const std::vector<std::string> &args) override {
        if (cmd != "minigames" && cmd != "endgames")
            return false;
        if (!args.empty()) {
            Error(TR("Используйте /minigames или /endgames без аргументов.",
                     "Use /minigames or /endgames without arguments."));
            return true;
        }
        if (!Net().Online()) {
            Error(TR("Сначала подключитесь к сессии.", "Join a session first."));
            return true;
        }
        if (cmd == "endgames" && !g_active) {
            Error(TR("Догонялки сейчас не запущены.", "Tag is not running."));
            return true;
        }
        if (cmd == "minigames" && !g_active) {
            auto reason = SafeReason();
            if (reason != tag::None) {
                LocalReason(reason);
                return true;
            }
        }
        Send(cmd == "minigames" ? tag::Start : tag::End);
        return true;
    }
    void OnPreProcess() override {
        if (!g_active)
            return;
        if (!core::IsPlayerReady() || core::IsRestarting() || GetPed(m_ped) != FindPlayerPed(0)) {
            Send(tag::Reject, tag::PlayerChanged);
            Finish(false);
            return;
        }
        if (!Net().Online() || NowMs() - g_received > 12000) {
            LocalReason(tag::ConnectionLost);
            LocalExit();
            return;
        }
        auto *me = FindPlayerPed(0);
        if (me->Health() <= 0 || me->AreaCode() != 0 || me->InVehicle() || core::IsOnMission() || cam::WideScreenOn() ||
            Ref<uint8_t>(0xB5F851)) {
            uint8_t reason = me->Health() <= 0     ? tag::Dead
                             : me->AreaCode() != 0 ? tag::Interior
                             : me->InVehicle()     ? tag::Vehicle
                             : core::IsOnMission() ? tag::Mission
                                                   : tag::Scene;
            Send(tag::Reject, reason);
            LocalExit();
            return;
        }
        if (NowMs() - m_pulse >= 400) {
            Send(tag::Pulse);
            m_pulse = NowMs();
        }
        bool freeze = g_state.phase == tag::Preparing || g_state.phase == tag::Moving ||
                      g_state.phase == tag::Restoring || g_state.phase == tag::Result ||
                      (g_state.phase == tag::Hiding && g_state.catcher == Net().MyId());
        if (freeze) {
            AcquirePlayerControlLock(kLock);
            me->MoveSpeed() = {};
            me->TurnSpeed() = {};
        } else
            ReleasePlayerControlLock(kLock);
        GetPad(0)->bDisablePlayerEnterCar = 1;
        RemoteRestrictions();
        if (g_state.phase != tag::Restoring) {
            SetTemporaryStamina(false);
            if (me->ActiveWeapon().type != 0)
                Cmd(0x01B9).I(m_ped).I(0).Run();
            // Stop wheel/shoulder weapon selection and vehicle entry without disabling punches or movement.
            auto &pad = *GetPad(0);
            pad.NewState.LeftShoulder2 = pad.NewState.RightShoulder2 = 0;
            pad.NewState.ButtonTriangle = 0;
            Cmd(0x0110).I(0).Run(); // CLEAR_WANTED_LEVEL; no wanted policy is changed
        }
        if (m_movePending) {
            m_movePending = false;
            auto failure = Move();
            if (failure != tag::None) {
                Send(tag::SpawnFailed, failure);
                return;
            }
            m_moved = true;
            Send(tag::Arrived);
        }
        if (m_restorePending) {
            m_restorePending = false;
            Restore();
            Send(tag::Restored);
        }
    }
    void OnPostProcess() override {
        if (!g_active || !core::IsPlayerReady() || GetPed(m_ped) != FindPlayerPed(0))
            return;
        auto *me = FindPlayerPed(0);
        if (g_state.phase != tag::Restoring) {
            me->Health() = std::max(100.0f, m_health);
            me->Armour() = m_armour;
        }
        RemoteRestrictions();
    }
    void OnSessionEnd() override { LocalExit(); }
    void OnPeerLeave(uint8_t) override {
        if (g_active) {
            Send(tag::Reject, tag::PeerUnavailable);
            LocalExit();
        }
    }
    void OnHostChanged(uint8_t) override {
        if (g_active) {
            Send(tag::Reject, tag::HostChanged);
            LocalExit();
        }
    }
    void OnGameShutdown() override {
        if (g_active) {
            Send(tag::Reject, tag::WorldShutdown);
            Finish(false);
        }
    }
    void OnPeerResync(uint8_t) override {
        if (g_active) {
            Send(tag::Reject, tag::PeerResynced);
            LocalExit();
        }
    }
    void ReportHit(CPed *victim, CEntity *attacker, int weapon) {
        if (g_state.phase != tag::Chasing || Net().MyId() != g_state.catcher || attacker != FindPlayerPed(0) ||
            (weapon != 0 && weapon != 1) || NowMs() - m_hit < 300)
            return;
        auto *rp = Players::Instance().FindByPed(victim);
        if (!rp || rp->id != g_state.runner || !rp->hasState || NowMs() - rp->stateRecvMs > 2000 ||
            (victim->GetPosition() - FindPlayerPed(0)->GetPosition()).Length() > 4.0f)
            return;
        m_hit = NowMs();
        Send(tag::Pulse);
        Send(tag::Hit);
    }

  private:
    int m_ped = -1, m_weapon = 0, m_money = 0, m_displayMoney = 0;
    CVector m_origin;
    float m_heading = 0, m_health = 100, m_armour = 0, m_sprintEnergy = 0;
    CWeapon m_weapons[13]{};
    uint8_t m_stats[kKeptStatsSize]{}, m_enterCar = 0;
    CPedClothesDesc m_clothes{};
    bool m_captured = false, m_moved = false, m_restored = false, m_movePending = false, m_restorePending = false;
    uint64_t m_pulse = 0, m_hit = 0;
    static void Error(const char *text) { ui::TheChat().AddError(text); }
    void LocalReason(uint8_t reason) {
        LOGW("Tag local check: %u %s (phase %u generation %u)", reason, tag::ReasonText(reason, true), g_state.phase,
             g_state.round);
        Error(
            Format(TR("Догонялки: %s.", "Tag: %s."), TR(tag::ReasonText(reason, false), tag::ReasonText(reason, true)))
                .c_str());
    }
    void StateReason(const tag::State &state) {
        if (state.reason == tag::None || state.reason == tag::UserCancelled)
            return;
        auto who = state.actor == 255 ? std::string("server") : Net().PeerName(state.actor);
        auto text = Format(TR("Догонялки — %s: %s.", "Tag — %s: %s."), who.c_str(),
                           TR(tag::ReasonText(state.reason, false), tag::ReasonText(state.reason, true)));
        if (state.phase == tag::Moving)
            ui::TheChat().AddSystem(text + TR(" Выбираем другую площадку.", " Selecting another location."));
        else
            Error(text.c_str());
    }
    void RemoteRestrictions() {
        for (const auto &[id, remote] : Players::Instance().All()) {
            auto *rp = Players::Instance().Get(id);
            if (!rp)
                continue;
            if (rp->blip && rp->tagBlipHidden != TagGameHidden(id)) {
                rp->tagBlipHidden = TagGameHidden(id);
                Cmd(0x018B).I(rp->blip).I(rp->tagBlipHidden ? 0 : 2).Run();
            }
            // Remote copies are normally melee-proof. Let the engine test real punch contacts;
            // the damage hooks still suppress HP loss before it can be applied.
            if (rp->ped && !rp->tagProofHeld) {
                rp->tagMeleeWasProof = (rp->ped->PhysicalFlags() & CPhysical::PF_MELEE_PROOF) != 0;
                rp->tagProofHeld = true;
                rp->ped->PhysicalFlags() &= ~CPhysical::PF_MELEE_PROOF;
            }
        }
    }
    void Send(uint8_t action, uint8_t reason = tag::None) {
        if (!Net().Online())
            return;
        Writer w(proto::C_TAG);
        w.U8(action);
        w.U64(g_state.session);
        w.U32(g_state.round);
        if (action == tag::Reject || action == tag::SpawnFailed) {
            w.U8(reason);
            LOGW("Tag report action %u phase %u generation %u location %s: %s", action, g_state.phase, g_state.round,
                 tag::locations[g_state.location].name, tag::ReasonText(reason, true));
        }
        if (action == tag::Pulse) {
            auto p = FindPlayerPed(0)->GetPosition();
            w.Vec({p.x, p.y, p.z});
        }
        Net().Send(proto::CH_RELIABLE, w);
    }
    uint8_t SafeReason() {
        if (!g_hooks)
            return tag::HooksUnavailable;
        if (!core::IsPlayerReady() || core::IsRestarting())
            return tag::ClientNotReady;
        if (!flow::InSharedWorld())
            return tag::WorldNotReady;
        if (!MoneyProfileReady())
            return tag::ProfileNotReady;
        if (core::IsOnMission() || HostMissionRecent(1500))
            return tag::Mission;
        if (flow::HostWorldLoading() || flow::HostWorldWaiting() || flow::IsFollower())
            return tag::WorldChanging;
        if (AtMissionMarker())
            return tag::MissionMarker;
        auto *me = FindPlayerPed(0);
        int wanted = 0;
        Cmd(0x01C0).I(0).Out(&wanted).Run(); // STORE_WANTED_LEVEL
        if (me->Health() <= 0)
            return tag::Dead;
        if (me->AreaCode() != 0)
            return tag::Interior;
        if (wanted)
            return tag::Wanted;
        if (me->InVehicle())
            return tag::Vehicle;
        if (me->IsInTheAir() || !(me->PedFlags() & 1))
            return tag::Airborne;
        if (PlayerAfk(Net().MyId()).active)
            return tag::Afk;
        if (PlayerInjury(Net().MyId()).Active())
            return tag::Injured;
        if (PlayerInfos()[0].playerState != 0)
            return tag::Busy;
        if (GetPad(0)->DisablePlayerControls) {
            LOGW("Tag controls blocked: %04X", GetPad(0)->DisablePlayerControls);
            return tag::ControlLock;
        }
        if (cam::WideScreenOn() || Ref<uint8_t>(0xB5F851))
            return tag::Scene;
        if (ActivePrimaryTaskType(me) != 0)
            return tag::ScriptedTask;
        return tag::None;
    }
    void SendTemporaryStats() {
        if (!Net().Online() || !core::IsPlayerReady() || core::IsRestarting())
            return;
        Writer w(proto::G_PLAYER_STATS); // peer simulation only; never C_PROFILE_SAVE
        w.Bytes((void *)0xB79000, kNetworkStatsSize);
        Net().Send(proto::CH_RELIABLE, w);
    }
    void SetTemporaryStamina(bool refill) {
        if (!m_captured || !core::IsPlayerReady() || core::IsRestarting() || GetPed(m_ped) != FindPlayerPed(0))
            return;
        Ref<float>(kStaminaStat) = 1000.0f;
        if (refill)
            CallMethod<0x60A530>(FindPlayerPed(0)); // CPlayerPed::ResetSprintEnergy, uses the boosted stat
    }
    void Capture() {
        SaveProfileNow(); // only the original state may reach the persistent profile
        auto *me = FindPlayerPed(0);
        m_ped = GetPedRef(me);
        m_origin = me->GetPosition();
        m_heading = me->CurrentRotation();
        m_health = me->Health();
        m_armour = me->Armour();
        m_weapon = me->ActiveWeapon().type;
        m_money = PlayerInfos()[0].money;
        m_displayMoney = PlayerInfos()[0].displayMoney;
        memcpy(m_weapons, me->Weapons(), sizeof m_weapons);
        memcpy(m_stats, (void *)0xB79000, sizeof m_stats);
        m_clothes = *me->ClothesDesc();
        m_sprintEnergy = Field<float>(me->PlayerData(), 0x18); // CPlayerData::m_fTimeCanRun
        m_enterCar = GetPad(0)->bDisablePlayerEnterCar;
        m_captured = true;
        m_restored = m_moved = false;
        g_scriptClock = Ref<uint32_t>(addr::TimeInMs);
        g_active = true;
        SetTemporaryStamina(true);
        SendTemporaryStats();
        AcquirePlayerControlLock(kLock);
    }
    uint8_t Move() {
        auto location = tag::locations[g_state.location];
        float x = location.x + (Net().MyId() == g_state.catcher ? -1.5f : 1.5f), y = location.y;
        Cmd(0x03CB).F(x).F(y).F(location.z).Run(); // LOAD_SCENE: collision must exist before teleport
        CVector target{x, y, location.z};
        Call<0x40ED80>(&target); // CStreaming::LoadSceneCollision: LOAD_SCENE alone does not load distant COL files
        bool found = false;
        float z = Call<0x5696C0, float>(x, y, location.z + 30.0f, &found, (CEntity **)nullptr);
        LOGI("Tag spawn %s x %.2f y %.2f expected z %.2f ground %.2f found %u", location.name, x, y, location.z, z,
             found);
        if (!found || !std::isfinite(z))
            return tag::GroundMissing;
        if (std::abs(z - location.z) > 25.0f)
            return tag::GroundHeight;
        float water = 0;
        if (Call<0x6EB690, bool>(x, y, z, &water, true, (CVector *)nullptr) && water > z - 0.3f)
            return tag::Water; // seabed is ground too; never spawn either player in water
        auto *me = FindPlayerPed(0);
        Cmd(0x0687).I(m_ped).Run();
        Cmd(0x00A1).I(m_ped).F(x).F(y).F(z + 0.1f).Run();
        me->MoveSpeed() = {};
        me->TurnSpeed() = {};
        Cmd(0x01B9).I(m_ped).I(0).Run();
        auto actual = me->GetPosition();
        if (std::abs(actual.x - x) > 3.0f || std::abs(actual.y - y) > 3.0f || std::abs(actual.z - z) > 4.0f)
            return tag::TeleportFailed;
        return tag::None;
    }
    void Restore() {
        if (!m_captured || m_restored || !core::IsPlayerReady() || core::IsRestarting() ||
            GetPed(m_ped) != FindPlayerPed(0))
            return;
        auto *me = FindPlayerPed(0);
        Cmd(0x0687).I(m_ped).Run();
        Cmd(0x04BB).I(0).Run();
        Cmd(0x0860).I(m_ped).I(0).Run();
        Cmd(0x03CB).F(m_origin.x).F(m_origin.y).F(m_origin.z).Run();
        Call<0x40ED80>(&m_origin);
        Cmd(0x00A1).I(m_ped).F(m_origin.x).F(m_origin.y).F(m_origin.z - 1.0f).Run();
        LightArea(0, m_origin);
        me->CurrentRotation() = me->AimingRotation() = m_heading;
        me->MoveSpeed() = {};
        me->TurnSpeed() = {};
        me->Health() = m_health;
        me->Armour() = m_armour;
        PlayerInfos()[0].money = m_money;
        PlayerInfos()[0].displayMoney = m_displayMoney;
        memcpy((void *)0xB79000, m_stats, sizeof m_stats);
        if (memcmp(me->ClothesDesc(), &m_clothes, sizeof m_clothes)) {
            *me->ClothesDesc() = m_clothes;
            Call<0x5A82C0>(me, false);
        }
        // Inventory never leaves the player: only fists can be selected and pickups/transfers are disabled.
        // If another plugin changed weapon types anyway, rebuild the original inventory as well.
        bool changed = false;
        for (int i = 0; i < 13; ++i)
            changed |= me->Weapons()[i].type != m_weapons[i].type;
        if (changed) {
            Cmd(0x048F).I(m_ped).Run();
            for (const auto &weapon : m_weapons)
                if (weapon.type > 0 && weapon.type <= 46) {
                    int model = -1;
                    Cmd(0x0781).I(weapon.type).Out(&model).Run();
                    if (model > 0)
                        LoadModelNow(model);
                    Cmd(0x01B2).I(m_ped).I(weapon.type).I(std::max(weapon.ammoTotal, 1)).Run();
                }
        }
        for (int i = 0; i < 13; ++i)
            if (me->Weapons()[i].type == m_weapons[i].type) {
                me->Weapons()[i].ammoTotal = m_weapons[i].ammoTotal;
                me->Weapons()[i].ammoInClip = m_weapons[i].ammoInClip;
            }
        Cmd(0x01B9).I(m_ped).I(m_weapon).Run();
        Field<float>(me->PlayerData(), 0x18) = m_sprintEnergy;
        m_restored = true;
        SendTemporaryStats();
    }
    void Finish(bool restored) {
        if (!g_active)
            return;
        if (restored)
            ShiftScriptWaits();
        g_active = false;
        ReleasePlayerControlLock(kLock);
        GetPad(0)->bDisablePlayerEnterCar = m_enterCar;
        for (const auto &[id, remote] : Players::Instance().All()) {
            auto *rp = Players::Instance().Get(id);
            if (rp && rp->blip && rp->tagBlipHidden) {
                Cmd(0x018B).I(rp->blip).I(2).Run();
                rp->tagBlipHidden = false;
            }
            if (rp && rp->ped && rp->tagProofHeld) {
                if (rp->tagMeleeWasProof)
                    rp->ped->PhysicalFlags() |= CPhysical::PF_MELEE_PROOF;
                else
                    rp->ped->PhysicalFlags() &= ~CPhysical::PF_MELEE_PROOF;
                rp->tagProofHeld = false;
            }
        }
        m_captured = m_moved = m_movePending = m_restorePending = false;
        m_ped = -1;
        if (restored)
            ui::TheChat().AddSystem(TR("Догонялки завершены. Вы вернулись на исходное место.",
                                       "Tag ended. You are back at your original position."));
    }
    void LocalExit() {
        if (g_active) {
            Restore();
            Finish(m_restored);
        }
    }
    void Receive(const tag::State &state) {
        if (g_active && state.session != g_state.session)
            return;
        // A participant that rejected PREPARE never teleported, but still takes part in the restore barrier.
        if (!g_active && state.session == g_state.session && state.phase == tag::Restoring) {
            if (g_state.phase != tag::Restoring)
                StateReason(state);
            g_state = state;
            Send(tag::Restored);
            return;
        }
        if (!g_active && state.phase != tag::Preparing)
            return;
        bool changed = state.phase != g_state.phase || state.round != g_state.round || state.session != g_state.session;
        g_state = state;
        g_received = NowMs();
        if (changed)
            LOGI("Tag state phase %u generation %u site %s reason %s actor %u", state.phase, state.round,
                 tag::locations[state.location].name, tag::ReasonText(state.reason, true), state.actor);
        if (state.phase == tag::Preparing && !g_active) {
            auto reason = SafeReason();
            if (reason != tag::None) {
                LocalReason(reason);
                Send(tag::Reject, reason);
                return;
            }
            Capture();
            Send(tag::Ready);
            return;
        }
        if (state.phase == tag::Idle) {
            LocalExit();
            return;
        }
        if (!changed)
            return;
        StateReason(state);
        if (state.phase == tag::Moving) {
            m_movePending = true;
            AcquirePlayerControlLock(kLock);
        }
        if (state.phase == tag::Restoring) {
            m_movePending = false;
            m_restorePending = true;
            AcquirePlayerControlLock(kLock);
        }
        if (state.phase == tag::Hiding) {
            SetTemporaryStamina(true);
            ui::TheChat().AddSystem(
                Net().MyId() == state.runner
                    ? TR("Вы убегаете! 35 секунд, чтобы спрятаться. Затем продержитесь 10 минут.",
                         "You are the runner! Hide for 35 seconds, then evade for 10 minutes.")
                    : TR("Вы догоняете. Подождите 35 секунд, затем поймайте игрока одним ударом кулаком.",
                         "You are the catcher. Wait 35 seconds, then land one punch."));
        }
        if (state.phase == tag::Result)
            ui::TheChat().AddSystem(Format(TR("Раунд окончен. Победил %s! Через 5 секунд смена ролей.",
                                              "Round over. %s wins! Roles swap in 5 seconds."),
                                           Net().PeerName(state.winner).c_str()));
    }
};
struct Register {
    Register() { core::RegisterModule(&TagGame::Instance()); }
} g_register;
} // namespace

bool TagGameActive() { return g_active; }
bool TagGameHidden(uint8_t id) {
    return g_active && id == g_state.runner &&
           (g_state.phase == tag::Moving || g_state.phase == tag::Hiding || g_state.phase == tag::Chasing);
}
bool TagGameBlind() {
    return g_active && (g_state.phase == tag::Moving || g_state.phase == tag::Restoring ||
                        (g_state.phase == tag::Hiding && Net().MyId() == g_state.catcher));
}
bool TagGameProtects(game::CPed *victim) {
    return g_active && victim && (victim == game::FindPlayerPed(0) || Players::Instance().FindByPed(victim));
}
bool TagGameHit(game::CPed *victim, game::CEntity *attacker, int weapon) {
    if (!g_active)
        return false;
    TagGame::Instance().ReportHit(victim, attacker, weapon);
    // Neither participant can damage story NPCs while their scripts are suspended.
    return TagGameProtects(victim) || attacker == game::FindPlayerPed(0) ||
           (attacker && attacker->IsPed() && Players::Instance().FindByPed((game::CPed *)attacker));
}
void DrawTagGameOverlay() {
    if (!g_active)
        return;
    auto &gfx = ui::Gfx();
    gfx.SetHidden(false);
    bool blind = TagGameBlind();
    if (blind)
        gfx.Rect(0, 0, gfx.Width(), gfx.Height(), 0xFF000000);
    else
        gfx.Rect(gfx.Width() / 2 - 250, 12, 500, 76, 0xC0000000);
    uint64_t elapsed = NowMs() - g_received;
    unsigned seconds = unsigned((g_state.remaining > elapsed ? g_state.remaining - elapsed : 0) + 999) / 1000;
    const char *title =
        g_state.phase == tag::Hiding
            ? (Net().MyId() == g_state.catcher ? TR("Не подглядывай! Беглец прячется", "No peeking! Runner is hiding")
                                               : TR("Спрячься!", "Find a hiding place!"))
        : g_state.phase == tag::Chasing
            ? (Net().MyId() == g_state.catcher ? TR("Поймай одним ударом кулаком", "Land one punch to win")
                                               : TR("Не дай себя поймать!", "Evade the catcher!"))
        : g_state.phase == tag::Result    ? TR("Раунд завершён — смена ролей", "Round over — swapping roles")
        : g_state.phase == tag::Restoring ? TR("Возвращаемся в обычную игру…", "Returning to the main game…")
                                          : TR("Подготовка раунда…", "Preparing round…");
    float y = blind ? gfx.Height() / 2 - 40 : 20;
    gfx.Text(ui::FONT_NORMAL, gfx.Width() / 2, y, title, 0xFFFFFFFF, ui::TEXT_CENTER);
    gfx.Text(ui::FONT_SMALL, gfx.Width() / 2, y + 30, Format("%02u:%02u  |  /endgames", seconds / 60, seconds % 60),
             0xFFFFCC66, ui::TEXT_CENTER);
}
} // namespace coop
