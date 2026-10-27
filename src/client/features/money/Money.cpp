#include "entities/session/TagGame.h"
#include "common/lib/money/Money.h"
#include "entities/profile/Profile.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop {
namespace {

class Money : public Module {
public:
    const char* Name() const override { return "Money"; }
    void OnInstallHooks() override { Net().SetHandler(proto::S_MONEY_APPLY, &Apply); }

    static Money& Instance() {
        static Money m;
        return m;
    }

private:
    money::Receipts m_receipts;

    static void Apply(uint8_t from, Reader& r) {
        uint64_t id = r.U64();
        uint8_t action = r.U8();
        int32_t amount = r.I32();
        auto other = r.Str(80);
        if (!r.Ok() || from != proto::kServerId || !id || amount <= 0 || action > proto::MONEY_REFUND) return;

        bool ready = !TagGameActive() && core::IsPlayerReady() && !core::IsRestarting() && flow::InSharedWorld() && MoneyProfileReady();
        // Even a duplicate received in a loading screen must ACK its cached outcome
        // without dereferencing the game's player memory.
        int32_t balance = ready ? game::PlayerInfos()[0].money : 0;
        int32_t before = balance;
        uint8_t result = Instance().m_receipts.Apply(id, action, amount, ready, balance);
        if (ready && result == proto::MONEY_OK && balance != before) {
            game::PlayerInfos()[0].money = balance;
            SaveProfileNow();  // retain it across the next host-world reload
        }
        Writer w(proto::C_MONEY_ACK);
        w.U64(id);
        w.U8(action);
        w.U8(result);
        Net().Send(proto::CH_RELIABLE, w);
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Money::Instance()); }
} g_reg;

}  // namespace
}  // namespace coop
