// Real ENet clients exercise the built server; wallet arithmetic is shared with
// the game feature. No game files, profiles or public network services are used.
#include <windows.h>
#include <enet/enet.h>
#include <cassert>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <array>

#include "common/config/version/Version.h"
#include "common/lib/money/Money.h"
#include "common/lib/afk/Afk.h"
#include "common/lib/gear/Gear.h"
#include "common/lib/rescue/Rescue.h"

using namespace coop;
using namespace coop::proto;

static void Require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

static void UnitTests() {
    int32_t v = -1;
    for (const char* bad : {"", "0", "-1", "+1", "1.0", "1e3", "1x", " 1", "2147483648", "9999999999999999999"})
        Require(!money::ParsePositive(bad, INT32_MAX, v), "invalid amount accepted");
    Require(money::ParsePositive("2147483647", INT32_MAX, v) && v == INT32_MAX, "max amount");
    Require(money::ParsePositive("00012", INT32_MAX, v) && v == 12, "leading zeroes");
    Require(money::ParsePositive("0", 3, v, true) && v == 0, "host id zero");
    Require(!money::ParsePositive("4", 3, v, true), "invalid player id");
    money::Receipts r;
    int32_t balance = 1000;
    Require(r.Apply(1, MONEY_DEBIT, 400, true, balance) == MONEY_OK && balance == 600, "debit");
    Require(r.Apply(1, MONEY_DEBIT, 400, true, balance) == MONEY_OK && balance == 600, "duplicate debit");
    Require(r.Apply(1, MONEY_DEBIT, 400, false, balance) == MONEY_OK && balance == 600, "duplicate during loading");
    Require(r.Apply(2, MONEY_DEBIT, 601, true, balance) == MONEY_REJECTED && balance == 600, "insufficient funds");
    balance = 2000;
    Require(r.Apply(2, MONEY_DEBIT, 601, true, balance) == MONEY_REJECTED && balance == 2000, "rejection is final");
    Require(r.Apply(3, MONEY_CREDIT, 50, false, balance) == MONEY_UNAVAILABLE, "loading recipient");
    Require(r.Apply(3, MONEY_CREDIT, 50, true, balance) == MONEY_UNAVAILABLE && balance == 2000, "late credit after rejection");
    balance = INT32_MAX;
    Require(r.Apply(4, MONEY_CREDIT, 1, true, balance) == MONEY_REJECTED && balance == INT32_MAX, "overflow credit");
    Require(r.Apply(5, MONEY_REFUND, 1, true, balance) == MONEY_BUSY, "refund waits for room");
    balance -= 1;
    Require(r.Apply(5, MONEY_REFUND, 1, true, balance) == MONEY_OK && balance == INT32_MAX, "refund retries");
    balance = -100;
    Require(r.Apply(6, MONEY_DEBIT, 1, true, balance) == MONEY_REJECTED && balance == -100, "debt cannot be spent");
    Require(r.Apply(7, MONEY_CREDIT, 10, true, balance) == MONEY_OK && balance == -90, "credit pays debt");
    Require(r.Apply(8, MONEY_DEBIT, -1, true, balance) == MONEY_REJECTED, "negative packet amount");
    std::cout << "Wallet/parser tests passed\n";
}

struct Actor {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    std::string name;
    uint8_t id = 255;
    int32_t balance = 0;
    bool ready = true, joined = false, dropNextAck = false, holdDebit = false;
    bool expectReject = false, rejected = false;
    uint32_t protocol = kProtocolVersion;
    uint64_t lastId = 0;
    uint64_t instanceId = 0;
    uint8_t lastAction = 255;
    unsigned operations = 0;
    money::Receipts receipts;
    std::vector<std::string> chats;
    afk::States away;
    std::array<gear::SlotState, 13> inventory{};
    gear::Receipts gearReceipts;
    bool gearReady = true, gearNear = true, dropGearAck = false, holdGearDebit = false;
    unsigned gearOps = 0;
    std::map<uint8_t, rescue::State> wounds;
    uint8_t reviveSender = 255, reviveTarget = 255, reviveAction = 255;
    uint64_t reviveEpisode = 0;

    Actor(const char* nick, int32_t cash) : name(nick), balance(cash) {
        FILETIME stamp; GetSystemTimeAsFileTime(&stamp);
        instanceId = (uint64_t(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime;
    }
    ~Actor() { Close(); }
    void Close() {
        if (peer) { enet_peer_disconnect_now(peer, 1); enet_host_flush(host); }
        if (host) enet_host_destroy(host);
        host = nullptr; peer = nullptr; joined = false; id = 255;
    }
    void Connect(uint16_t port) {
        host = enet_host_create(nullptr, 1, CH_COUNT, 0, 0);
        Require(host != nullptr, "client host");
        enet_host_compress_with_range_coder(host);
        ENetAddress addr{};
        enet_address_set_host(&addr, "127.0.0.1");
        addr.port = port;
        peer = enet_host_connect(host, &addr, CH_COUNT, 0);
        Require(peer != nullptr, "client peer");
    }
    void Send(const Writer& w) {
        enet_peer_send(peer, CH_RELIABLE, enet_packet_create(w.Data(), w.Size(), ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(host);
    }
    void Status(uint8_t st) { Writer w(C_STATUS); w.U8(st); Send(w); }
    void Chat(const std::string& s) { Writer w(C_CHAT); w.Str(s); Send(w); }
    bool Has(const std::string& s) const {
        for (auto& c : chats) if (c.find(s) != std::string::npos) return true;
        return false;
    }
    void Poll() {
        if (!host) return;
        ENetEvent ev;
        while (enet_host_service(host, &ev, 0) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                Hello h;
                h.protocol = protocol; h.version = COOP_VERSION_STR; h.name = name;
                h.wantsHost = name == "Host"; h.exeSize = kSupportedExeSize;
                h.instanceId = instanceId;
                Writer w(C_HELLO); h.Write(w); Send(w);
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                Reader r(ev.packet->data, ev.packet->dataLength);
                uint8_t msg = r.U8();
                if (msg == S_WELCOME) {
                    Welcome w;
                    Require(w.Read(r), "welcome"); id = w.yourId; joined = true; Status(STATUS_INGAME);
                } else if (msg == S_CHAT) {
                    r.U8(); r.U32(); auto ru = r.Str(); auto en = r.Str();
                    chats.push_back(en.empty() ? ru : en);
                } else if (msg == G_PLAYER_AFK) {
                    uint8_t from = r.U8();
                    afk::State state;
                    if (state.Read(r)) away.Set(from, state);
                } else if (msg == G_PLAYER_INJURY) {
                    uint8_t from = r.U8(); rescue::State state;
                    if (state.Read(r)) wounds[from] = state;
                } else if (msg == G_REVIVE_ACTION) {
                    reviveSender = r.U8(); reviveTarget = r.U8(); reviveEpisode = r.U64(); reviveAction = r.U8();
                } else if (msg == S_PLAYER_LEAVE) {
                    auto from = r.U8(); away.Remove(from); wounds.erase(from);
                } else if (msg == S_MONEY_APPLY) {
                    lastId = r.U64(); lastAction = r.U8(); int32_t amount = r.I32(); r.Str();
                    Require(r.Ok(), "money packet");
                    ++operations;
                    if (!(holdDebit && lastAction == MONEY_DEBIT)) {
                        auto result = receipts.Apply(lastId, lastAction, amount, ready, balance);
                        if (dropNextAck) dropNextAck = false;
                        else { Writer w(C_MONEY_ACK); w.U64(lastId); w.U8(lastAction); w.U8(result); Send(w); }
                    }
                } else if (msg == S_GEAR_APPLY) {
                    lastId = r.U64(); lastAction = r.U8(); int32_t ammo = r.I32(); r.Str();
                    gear::Item item; item.kind = r.U8(); item.weapon = r.U8(); item.ammo = ammo;
                    Require(r.Ok() && item.Valid(), "gear packet"); ++gearOps;
                    if (!(holdGearDebit && lastAction == gear::Debit)) {
                        uint8_t result = gearReceipts.Apply(lastId, lastAction, item, gearReady, gearNear, inventory[gear::Slot(item.weapon)]);
                        if (dropGearAck) dropGearAck = false;
                        else { Writer w(C_GEAR_ACK); w.U64(lastId); w.U8(lastAction); w.U8(result); Send(w); }
                    }
                } else if (msg == S_REJECT) {
                    auto reason = r.Str(); auto en = r.Str();
                    if (!expectReject) throw std::runtime_error("join rejected: " + reason);
                    Require(en.find("version") != std::string::npos, "legacy rejected for wrong reason");
                    rejected = true;
                }
                enet_packet_destroy(ev.packet);
            }
        }
    }
};

static void Wait(Actor& a, Actor& b, const std::function<bool()>& pred, const char* what, DWORD timeout = 6000) {
    auto end = GetTickCount64() + timeout;
    while (GetTickCount64() < end) {
        a.Poll(); b.Poll();
        if (pred()) return;
        Sleep(2);
    }
    throw std::runtime_error(std::string("timeout: ") + what);
}

static void Integration(uint16_t port) {
    Actor a("Host", 10000), b("Partner", 1000);
    a.Connect(port);
    Wait(a, b, [&] { return a.joined; }, "host join");
    b.Connect(port);
    Wait(a, b, [&] { return b.joined; }, "partner join");
    auto setAfk = [&](Actor& actor, afk::State state) {
        Writer w(G_PLAYER_AFK); state.Write(w); actor.Send(w);
    };
    a.Chat("/help");
    Wait(a, b, [&] { return a.Has("/afk"); }, "AFK help");
    setAfk(a, {true, 2});
    Wait(a, b, [&] { return b.away.Get(a.id).active; }, "AFK enable relay");
    Require(b.away.Get(a.id).phrase == 2 && !b.away.Get(b.id).active, "AFK phrase/ownership");
    Writer invalidAfk(G_PLAYER_AFK); invalidAfk.U8(1); invalidAfk.U8(255); a.Send(invalidAfk);
    a.Chat("relay barrier");
    Wait(a, b, [&] { return b.Has("relay barrier"); }, "malformed AFK barrier");
    Require(b.away.Get(a.id).active && b.away.Get(a.id).phrase == 2, "invalid AFK changed state");
    setAfk(b, {true, 1});
    Wait(a, b, [&] { return a.away.Get(b.id).active; }, "reverse AFK relay");
    setAfk(a, {}); setAfk(b, {});
    Wait(a, b, [&] { return !b.away.Get(a.id).active && !a.away.Get(b.id).active; }, "AFK toggle off relay");
    auto pay = [&](Actor& from, Actor& to, int32_t n) {
        from.Chat("/pay " + std::to_string(to.id) + " " + std::to_string(n));
    };
    auto give = [&](Actor& from, Actor& to, uint8_t kind, uint8_t weapon, int count) {
        Writer w(C_GEAR_REQUEST); w.U8(to.id); w.U8(kind); w.U8(weapon); w.I32(count); from.Send(w);
    };
    a.inventory[2] = {22, 500, 17}; b.inventory[2] = {22, 100, 7};
    a.chats.clear(); give(a, b, gear::Ammo, 22, 50);
    Wait(a, b, [&] { return a.Has("You gave 50 rounds"); }, "ammo transfer");
    Require(a.inventory[2].ammo == 450 && b.inventory[2].ammo == 150 && b.inventory[2].clip == 7, "ammo counts/reload");
    a.chats.clear(); a.dropGearAck = true; b.dropGearAck = true; give(a, b, gear::Gun, 22, 450);
    Wait(a, b, [&] { return a.Has("You gave your weapon"); }, "gun and lost gear ACKs");
    Require(a.inventory[2].weapon == 0 && b.inventory[2].ammo == 600, "gun was duplicated/lost");
    b.chats.clear(); give(b, a, gear::Gun, 22, 600);
    Wait(a, b, [&] { return b.Has("You gave your weapon"); }, "reverse gun");
    b.inventory[2] = {23, 100, 10}; a.chats.clear(); give(a, b, gear::Gun, 22, 600);
    Wait(a, b, [&] { return a.Has("Everything was refunded"); }, "occupied weapon slot refund");
    Require(a.inventory[2].weapon == 22 && a.inventory[2].ammo == 600 && b.inventory[2].weapon == 23 && b.inventory[2].ammo == 100, "refund replaced recipient's weapon");
    b.inventory[2] = {22, gear::kMaxAmmo, 7}; a.chats.clear(); give(a, b, gear::Ammo, 22, 1);
    Wait(a, b, [&] { return a.Has("Everything was refunded"); }, "ammo cap refund");
    Require(a.inventory[2].ammo == 600 && b.inventory[2].ammo == gear::kMaxAmmo, "ammo overflow lost rounds");
    b.inventory[2] = {22, 0, 0}; b.gearReady = false; a.chats.clear(); give(a, b, gear::Ammo, 22, 50);
    Wait(a, b, [&] { return a.Has("Everything was refunded"); }, "unavailable recipient refund"); b.gearReady = true;
    b.gearNear = false; a.chats.clear(); give(a, b, gear::Ammo, 22, 50);
    Wait(a, b, [&] { return a.Has("Everything was refunded"); }, "recipient walked away refund"); b.gearNear = true;
    Require(a.inventory[2].ammo == 600 && b.inventory[2].ammo == 0, "rejected recipient changed inventory");
    a.chats.clear(); give(a, b, gear::Ammo, 22, 601);
    Wait(a, b, [&] { return a.Has("Transfer cancelled"); }, "insufficient gear ammo");
    a.chats.clear(); a.holdGearDebit = true; auto oldGear = a.gearOps; give(a, b, gear::Ammo, 22, 10);
    Wait(a, b, [&] { return a.gearOps > oldGear; }, "held gear debit");
    Writer wrongAck(C_MONEY_ACK); wrongAck.U64(a.lastId); wrongAck.U8(gear::Debit); wrongAck.U8(gear::Ok); a.Send(wrongAck);
    pay(a, b, 1);
    Wait(a, b, [&] { return a.Has("previous transfer"); }, "gear blocks simultaneous money transfer");
    Require(a.inventory[2].ammo == 600 && b.inventory[2].ammo == 0, "money ACK confirmed gear transfer");
    a.holdGearDebit = false;
    Wait(a, b, [&] { return a.Has("You gave 10 rounds"); }, "held gear released");
    Require(a.inventory[2].ammo == 590 && b.inventory[2].ammo == 10, "gear conservation after held debit");
    a.chats.clear(); auto invalidOps = a.gearOps + b.gearOps; give(a, b, gear::Gun, 46, 1); a.Chat("invalid gear barrier");
    Wait(a, b, [&] { return a.Has("invalid gear barrier"); }, "invalid gear barrier");
    Require(a.gearOps + b.gearOps == invalidOps, "invalid equipment accepted");

    Writer injury(G_PLAYER_INJURY); rescue::State{999, rescue::Downed, 60, 255}.Write(injury); a.Send(injury);
    Wait(a, b, [&] { return b.wounds.count(a.id) && b.wounds[a.id].Active(); }, "downed state relay");
    Writer help(G_REVIVE_ACTION); help.U8(a.id); help.U64(999); help.U8(0); b.Send(help);
    Wait(a, b, [&] { return a.reviveSender == b.id && a.reviveTarget == a.id && a.reviveEpisode == 999 && a.reviveAction == 0; }, "revive request sender routing");
    Writer helped(G_PLAYER_INJURY); rescue::State{999, rescue::Revived, 0, 255}.Write(helped); a.Send(helped);
    Wait(a, b, [&] { return b.wounds[a.id].stage == rescue::Revived; }, "revived state relay");
    a.Chat("/help");
    Wait(a, b, [&] { return a.Has("/pay"); }, "help");
    a.chats.clear(); pay(a, b, 250);
    Wait(a, b, [&] { return a.Has("You sent $250") && b.Has("You received $250"); }, "forward payment");
    Require(a.balance == 9750 && b.balance == 1250, "forward balances");
    a.chats.clear(); b.chats.clear(); pay(b, a, 1250);
    Wait(a, b, [&] { return b.Has("You sent $1250"); }, "reverse/all funds payment");
    Require(a.balance == 11000 && b.balance == 0, "reverse balances");
    b.chats.clear(); pay(b, a, 1);
    Wait(a, b, [&] { return b.Has("not have enough"); }, "insufficient balance");
    Require(a.balance == 11000 && b.balance == 0, "insufficient preserved balances");
    for (const char* invalid : {"/pay", "/pay 1 0", "/pay 1 -1", "/pay 1 1.5", "/pay 1 2147483648", "/pay 1 2 extra", "/pay 1x 2", "/pay -1 2", "/pay 4 2"}) {
        a.chats.clear(); unsigned ops = a.operations + b.operations; a.Chat(invalid);
        Wait(a, b, [&] { return a.Has("Use /pay"); }, "syntax rejected");
        Require(a.operations + b.operations == ops, "invalid command reached wallet");
    }
    a.chats.clear(); a.Chat("/pay " + std::to_string(a.id) + " 1");
    Wait(a, b, [&] { return a.Has("cannot pay yourself"); }, "self pay");
    a.chats.clear(); a.Chat("/pay 3 1");
    Wait(a, b, [&] { return a.Has("no such player"); }, "missing player");
    b.Status(STATUS_MENU); b.Chat("/players");
    Wait(a, b, [&] { return b.Has("in the menu"); }, "menu status applied");
    a.chats.clear(); pay(a, b, 100);
    Wait(a, b, [&] { return a.Has("Both players"); }, "menu pay rejected");
    b.Status(STATUS_INGAME);
    b.chats.clear(); b.Chat("/players");
    Wait(a, b, [&] {
        for (auto& c : b.chats) if (c.find("Partner") != std::string::npos && c.find("in the game") != std::string::npos) return true;
        return false;
    }, "game status restored");

    b.balance = INT32_MAX; a.chats.clear(); pay(a, b, 100);
    Wait(a, b, [&] { return a.Has("refunded"); }, "overflow refund");
    Require(a.balance == 11000 && b.balance == INT32_MAX, "refund preserved total");
    b.balance = 0; b.ready = false; a.chats.clear(); pay(a, b, 100);
    Wait(a, b, [&] { return a.Has("refunded"); }, "loading refund");
    Require(a.balance == 11000 && b.balance == 0, "loading preserved total"); b.ready = true;

    a.dropNextAck = true; a.chats.clear(); pay(a, b, 150);
    Wait(a, b, [&] { return a.Has("You sent $150"); }, "debit ACK retry");
    Require(a.balance == 10850 && b.balance == 150, "debit retry duplicated money");
    b.dropNextAck = true; a.chats.clear(); pay(a, b, 200);
    Wait(a, b, [&] { return a.Has("You sent $200"); }, "credit ACK retry");
    Require(a.balance == 10650 && b.balance == 350, "credit retry duplicated money");

    // A wrong participant cannot confirm the sender's debit.
    a.holdDebit = true; a.chats.clear(); auto oldOps = a.operations; pay(a, b, 25);
    Wait(a, b, [&] { return a.operations > oldOps; }, "held debit");
    Writer forged(C_MONEY_ACK); forged.U64(a.lastId); forged.U8(MONEY_DEBIT); forged.U8(MONEY_OK); b.Send(forged);
    a.Chat("/pay " + std::to_string(b.id) + " 1");
    Wait(a, b, [&] { return a.Has("previous transfer"); }, "concurrent pay blocked");
    Require(a.balance == 10650 && b.balance == 350, "forged ACK moved money");
    a.holdDebit = false;
    Wait(a, b, [&] { return a.Has("You sent $25"); }, "held debit released");

    // Lose a credit ACK and reconnect under another player slot. The original
    // nickname owns the pending credit, and a receipt prevents a second credit.
    a.chats.clear(); b.dropNextAck = true; auto oldCredit = b.operations; pay(a, b, 75);
    Wait(a, b, [&] { return b.operations > oldCredit; }, "credit before disconnect");
    int32_t credited = b.balance; b.Close();
    Wait(a, b, [&] { return a.Has("waiting for the player to reconnect"); }, "disconnect notice");
    b.Connect(port);
    Wait(a, b, [&] { return b.joined && a.Has("You sent $75"); }, "resume after reconnect");
    Require(b.balance == credited && a.balance + b.balance == 11000, "reconnect duplicated credit");

    a.ready = false; a.chats.clear(); pay(a, b, 1);
    Wait(a, b, [&] { return a.Has("after the shared world"); }, "sender not ready");
    Require(a.balance + b.balance == 11000, "not-ready sender moved money"); a.ready = true;

    // A restarted process has no receipts. It must not receive a replay whose
    // outcome is uncertain, even under the original nickname.
    a.chats.clear(); b.dropNextAck = true; oldCredit = b.operations; pay(a, b, 50);
    Wait(a, b, [&] { return b.operations > oldCredit; }, "credit before restart");
    credited = b.balance; b.Close();
    Wait(a, b, [&] { return a.Has("waiting for the player to reconnect"); }, "restart disconnect notice");
    ++b.instanceId; b.receipts = money::Receipts{}; b.Connect(port);
    Wait(a, b, [&] { return a.Has("Automatic replay stopped"); }, "new process blocked");
    Require(b.balance == credited && a.balance + b.balance == 11000, "restart duplicated money");
    b.Close();
    Actor old("Legacy", 100); old.protocol = kProtocolVersion - 1; old.expectReject = true; old.Connect(port);
    Wait(a, old, [&] { return old.rejected; }, "legacy protocol rejected");
    std::cout << "Server ENet integration tests passed\n";
}

int main(int argc, char** argv) {
    try {
        UnitTests();
        Require(enet_initialize() == 0, "ENet init");
        if (argc > 1) Integration(static_cast<uint16_t>(std::stoi(argv[1])));
        enet_deinitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
