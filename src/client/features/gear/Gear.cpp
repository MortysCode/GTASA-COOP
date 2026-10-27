#include "entities/session/TagGame.h"
#include <algorithm>
#include "common/lib/gear/Gear.h"
#include "common/lib/money/Money.h"
#include "common/lib/util/Util.h"
#include "entities/player/Afk.h"
#include "entities/player/Injury.h"
#include "entities/player/Players.h"
#include "entities/profile/Profile.h"
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

bool Available() {
    if (TagGameActive()) return false;
    if (!Net().Online() || !flow::InSharedWorld() || !core::IsPlayerReady() || core::IsRestarting() || !MoneyProfileReady()) return false;
    auto* me = FindPlayerPed(0);
    return me->Health() > 0 && !me->InVehicle() && !me->IsInTheAir() && (me->PedFlags() & 1) && PlayerInfos()[0].playerState == 0 &&
           !PlayerAfk(Net().MyId()).active && !PlayerInjury(Net().MyId()).Active() && !GetPad(0)->DisablePlayerControls &&
           !cam::WideScreenOn() && !Ref<uint8_t>(0xB5F851);
}
bool Nearby(const RemotePlayer& other) {
    auto* me = FindPlayerPed(0);
    return me && other.ped && other.hasState && NowMs() - other.stateRecvMs < 2000 && other.state.health > 0 &&
           !(other.state.flags & OnFootState::F_DEAD) && other.mode == RemotePlayer::Mode::OnFoot &&
           other.state.area == me->AreaCode() && (other.state.pos - me->GetPosition()).Length() <= 3.0f &&
           !PlayerAfk(other.id).active && !PlayerInjury(other.id).Active();
}
bool ModelsReady(int type) {
    void* info = Call<0x743C60, void*>(type, uint8_t(1));
    if (!info || Field<int32_t>(info, 0x14) != gear::Slot(type)) return false;
    bool loaded = true;
    for (int offset : {0x0C, 0x10}) {
        int model = Field<int32_t>(info, offset);
        if (model > 0 && !IsModelLoaded(model)) { RequestModel(model); loaded = false; }
    }
    return loaded;
}

class Gear : public Module {
public:
    const char* Name() const override { return "Gear"; }
    void OnInstallHooks() override {
        Net().SetHandler(proto::S_GEAR_APPLY, [](uint8_t from, Reader& r) { Instance().Apply(from, r); });
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd != "givegun" && cmd != "giveammo") return false;
        auto error = [&](const char* text) { ui::TheChat().AddError(text); return true; };
        int32_t amount = 0;
        bool ammo = cmd == "giveammo";
        if ((!ammo && !args.empty()) || (ammo && (args.size() != 1 || !money::ParsePositive(args[0], gear::kMaxAmmo, amount))))
            return error(TR("Используйте /givegun или /giveammo <число>, например /giveammo 50.",
                            "Use /givegun or /giveammo <amount>, for example /giveammo 50."));
        if (!Available()) return error(TR("Для передачи встаньте на землю и завершите AFK или другие действия.",
                                         "To transfer equipment, stand on the ground and finish AFK or other activities."));
        if (m_requested && NowMs() - m_requested < 1500) return error(TR("Дождитесь ответа на предыдущую передачу.", "Wait for the previous transfer to respond."));
        auto& held = FindPlayerPed(0)->ActiveWeapon();
        gear::Item item{ammo ? uint8_t(gear::Ammo) : uint8_t(gear::Gun), static_cast<uint8_t>(held.type), ammo ? amount : held.ammoTotal};
        if (!item.Valid()) return error(TR("Возьмите передаваемое оружие в руки. Это снаряжение передать нельзя.",
                                          "Hold a transferable weapon in your hands. This equipment cannot be transferred."));
        if (held.ammoTotal < item.ammo) return error(TR("У вас недостаточно патронов.", "You do not have enough ammunition."));
        const RemotePlayer* target = nullptr;
        for (const auto& [id, other] : Players::Instance().All()) if (Nearby(other)) { target = &other; break; }
        if (!target) return error(TR("Подойдите к живому напарнику на расстояние до 3 метров, в одном помещении.",
                                    "Stand within 3 metres of your living partner in the same interior."));
        Writer w(proto::C_GEAR_REQUEST);
        w.U8(target->id); w.U8(item.kind); w.U8(item.weapon); w.I32(item.ammo);
        Net().Send(proto::CH_RELIABLE, w); m_requested = NowMs();
        return true;
    }
    static Gear& Instance() { static Gear g; return g; }
private:
    gear::Receipts m_receipts;
    uint64_t m_requested = 0;
    std::map<uint64_t, uint64_t> m_loadSince;
    uint64_t m_refundNotice = 0;

    void Apply(uint8_t from, Reader& r) {
        uint64_t id = r.U64(); uint8_t action = r.U8(); int32_t count = r.I32();
        auto otherName = r.Str(80); gear::Item item; item.kind = r.U8(); item.weapon = r.U8(); item.ammo = count;
        if (!r.Ok() || r.Remaining() || from != proto::kServerId || !id || action > gear::Refund || !item.Valid()) return;
        bool ready = Available();
        bool nearby = false;
        if (ready)
            for (const auto& [pid, other] : Players::Instance().All())
                if (ToLower(Net().PeerName(pid)) == ToLower(otherName) && Nearby(other)) nearby = true;
        // A refund does not depend on the other player still being present.
        // Model streaming stays asynchronous. A failure to load eventually refuses a credit.
        bool needsModel = action != gear::Debit && ready;
        bool models = !needsModel || ModelsReady(item.weapon);
        if (!models) {
            if (!m_loadSince.count(id)) m_loadSince[id] = NowMs();
            if (action == gear::Refund || NowMs() - m_loadSince[id] < 8000) { Ack(id, action, gear::Busy); return; }
            ready = false;
        }
        gear::SlotState state;
        CPlayerPed* me = ready ? FindPlayerPed(0) : nullptr;
        if (me) {
            auto& w = me->Weapons()[gear::Slot(item.weapon)];
            state = {w.type, w.ammoTotal, w.ammoInClip};
            if (action == gear::Debit && me->ActiveWeapon().type != item.weapon) state.weapon = -1;
        }
        auto before = state;
        uint8_t result = m_receipts.Apply(id, action, item, ready, nearby, state);
        if (action == gear::Refund && result == gear::Busy && ready && NowMs() - m_refundNotice > 5000) {
            m_refundNotice = NowMs();
            ui::TheChat().AddError(TR("Возврат снаряжения ожидает места в инвентаре. Освободите эту категорию оружия или потратьте часть патронов.",
                                      "The equipment refund needs inventory room. Free the weapon category or use some ammunition."));
        }
        if (me && result == gear::Ok && !(before == state)) {
            int handle = GetPedRef(me);
            if (!state.weapon) Cmd(0x0555).I(handle).I(item.weapon).Run();  // REMOVE_WEAPON_FROM_CHAR
            else {
                if (before.weapon != state.weapon) Cmd(0x01B2).I(handle).I(state.weapon).I(std::max(state.ammo, 1)).Run();
                Cmd(0x017B).I(handle).I(state.weapon).I(state.ammo).Run();
                auto& w = me->Weapons()[gear::Slot(item.weapon)];
                w.ammoTotal = state.ammo;
                w.ammoInClip = std::clamp(before.weapon != state.weapon && action == gear::Credit ? w.ammoInClip : state.clip, 0, state.ammo);
            }
            SaveProfileNow();
        }
        Ack(id, action, result);
    }
    void Ack(uint64_t id, uint8_t action, uint8_t result) {
        Writer w(proto::C_GEAR_ACK); w.U64(id); w.U8(action); w.U8(result); Net().Send(proto::CH_RELIABLE, w);
    }
};
struct Reg { Reg() { core::RegisterModule(&Gear::Instance()); } } g_reg;
}  // namespace
}  // namespace coop
