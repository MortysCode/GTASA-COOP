#include <windows.h>
#include <enet/enet.h>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "common/lib/tag/Tag.h"
#include "common/api/protocol/Protocol.h"
#include "common/config/version/Version.h"
using namespace coop;
using namespace coop::proto;
void Check(bool value, const char *text) {
    if (!value)
        throw std::runtime_error(text);
}
void Unit() {
    tag::Match m;
    m.Begin(100, 0, 1, 1000);
    Check(m.Active() && m.state.phase == tag::Preparing, "prepare");
    Check(!m.Ack(3) && !m.Ack(0) && !m.Ack(0) && m.Ack(1), "two distinct participant ACKs required");
    Check(!m.Tick(15999) && m.Tick(16000) && m.state.phase == tag::Restoring, "prepare timeout restores");
    m.Begin(101, 0, 1, 20000);
    uint64_t now = 20000;
    for (unsigned i = 0; i < 200; ++i) {
        auto old = m.state;
        m.Next(now, i * 93281);
        Check(m.state.round == i + 1 && m.state.location < tag::locationCount, "round and location");
        if (i)
            Check(m.state.catcher == old.runner && m.state.runner == old.catcher && m.state.location != old.location,
                  "alternation and no repeated location");
        Check(!m.Catch(m.state.catcher, now), "cannot catch while moving");
        Check(!m.Ack(m.state.runner) && !m.Ack(m.state.runner) && m.Ack(m.state.catcher), "move barrier");
        m.Set(tag::Hiding, now, 35000);
        Check(!m.Catch(m.state.catcher, now + 1), "cannot catch during hiding");
        Check(!m.Tick(now + 34999) && m.Tick(now + 35000) && m.state.phase == tag::Chasing, "exact hide timer");
        now += 35000;
        Writer w;
        m.Wire(now).Write(w);
        Reader r(w.Data(), w.Size());
        tag::State s;
        Check(s.Read(r) && s.remaining == 600000 && s.session == 101, "600 second state wire format");
        Check(!m.Catch(m.state.runner, now + 1), "runner cannot claim catch");
        if (i % 2) {
            Check(!m.Tick(now + 599999), "chase lasts full 10 minutes");
            Check(!m.Catch(m.state.catcher, now + 600000), "late hit cannot override timeout");
            Check(m.Tick(now + 600000) && m.state.winner == m.state.runner, "runner wins timeout");
            now += 600000;
        } else {
            Check(m.Catch(m.state.catcher, now + 20) && m.state.winner == m.state.catcher, "catcher wins hit");
            Check(!m.Catch(m.state.catcher, now + 21), "duplicate hit ignored");
            now += 20;
        }
        Check(m.state.phase == tag::Result, "result phase");
        now += 5000;
    }
    m.Set(tag::Moving, now, 20000);
    Check(m.Tick(now + 20000) && m.state.phase == tag::Restoring, "failed teleport timeout");
    Check(m.state.reason == tag::MoveTimeout, "move timeout must explain cancellation");
    m.Begin(102, 0, 1, now);
    m.Next(now, 5);
    auto first = m.state;
    uint32_t seen = 1u << first.location;
    Check(!m.Ack(first.catcher), "only one player arrived before failed spawn");
    for (unsigned attempt = 2; attempt <= 4; ++attempt) {
        auto oldRound = m.state.round;
        Check(m.RetryLocation(now + attempt, 9), "retry alternate location");
        Check(m.state.round == oldRound + 1 && !m.acknowledgements, "retry must invalidate old arrival ACKs");
        Check(m.state.catcher == first.catcher && m.state.runner == first.runner, "retry must preserve roles");
        Check(!(seen & (1u << m.state.location)), "retry repeated failed location");
        seen |= 1u << m.state.location;
    }
    Check(!m.RetryLocation(now + 5, 3), "retry budget must be bounded");
    m.Set(tag::Result, now, 5000);
    m.Next(now + 5000, 7);
    Check(m.state.catcher == first.runner && m.state.runner == first.catcher,
          "normal next round swaps roles once after retries");
    Check(!(m.failedLocations & (1u << m.state.location)), "next round selected known bad location");
    m.state.reason = tag::Water;
    m.state.actor = 1;
    Writer diagnostic;
    m.Wire(now + 5000).Write(diagnostic);
    Reader diagReader(diagnostic.Data(), diagnostic.Size());
    tag::State diag;
    Check(diag.Read(diagReader) && diag.reason == tag::Water && diag.actor == 1, "reason/actor state wire format");
    Check(tag::InReach(0, 0, 0, 3, 4, 0) && !tag::InReach(0, 0, 0, 3, 4, 1), "server catch distance boundary");
    Check(!tag::InReach(0, 0, 0, std::numeric_limits<float>::quiet_NaN(), 0, 0), "NaN coordinates rejected");
    Writer bad;
    bad.U8(1);
    Reader shortReader(bad.Data(), bad.Size());
    tag::State invalid;
    Check(!invalid.Read(shortReader), "truncated state rejected");
    std::cout << "Tag timers, role alternation, barriers, stale/late hits, wire and timeout tests passed\n";
}
struct Actor {
    ENetHost *host = nullptr;
    ENetPeer *peer = nullptr;
    uint8_t id = 255;
    tag::State state;
    bool joined = false;
    std::string name;
    unsigned states = 0, transfers = 0;
    std::string chat;
    std::string profile;
    bool profileReceived = false;
    Actor(const char *name) : name(name) {}
    ~Actor() { Close(); }
    void Close() {
        if (peer) {
            enet_peer_disconnect_now(peer, 0);
            enet_host_flush(host);
        }
        if (host)
            enet_host_destroy(host);
        peer = nullptr;
        host = nullptr;
        joined = false;
        profileReceived = false;
    }
    void Connect(uint16_t port) {
        host = enet_host_create(nullptr, 1, CH_COUNT, 0, 0);
        Check(host, "client host");
        enet_host_compress_with_range_coder(host);
        ENetAddress a{};
        enet_address_set_host(&a, "127.0.0.1");
        a.port = port;
        peer = enet_host_connect(host, &a, CH_COUNT, 0);
        Check(peer, "connect");
    }
    void Send(const Writer &w) {
        enet_peer_send(peer, CH_RELIABLE, enet_packet_create(w.Data(), w.Size(), ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(host);
    }
    void Action(uint8_t a, uint64_t token = 0, uint32_t round = UINT32_MAX, Vec3 pos = {0, 0, 0},
                uint8_t reason = tag::Busy) {
        Writer w(C_TAG);
        w.U8(a);
        w.U64(token ? token : state.session);
        w.U32(round == UINT32_MAX ? state.round : round);
        if (a == tag::Pulse)
            w.Vec(pos);
        if (a == tag::Reject || a == tag::SpawnFailed)
            w.U8(reason);
        Send(w);
    }
    void Chat(const std::string &text) {
        Writer w(C_CHAT);
        w.Str(text);
        Send(w);
    }
    void Profile(const std::string &text) {
        Writer w(C_PROFILE_SAVE);
        w.Bytes(text.data(), text.size());
        Send(w);
    }
    void Poll() {
        if (!host)
            return;
        ENetEvent ev;
        while (enet_host_service(host, &ev, 0) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                Hello h;
                h.protocol = kProtocolVersion;
                h.version = COOP_VERSION_STR;
                h.name = name;
                h.wantsHost = name == "Host";
                h.exeSize = kSupportedExeSize;
                h.instanceId = name == "Host" ? 101 : 202;
                Writer w(C_HELLO);
                h.Write(w);
                Send(w);
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                Reader r(ev.packet->data, ev.packet->dataLength);
                auto message = r.U8();
                if (message == S_WELCOME) {
                    Welcome w;
                    Check(w.Read(r), "welcome");
                    id = w.yourId;
                    joined = true;
                    Writer st(C_STATUS);
                    st.U8(STATUS_INGAME);
                    Send(st);
                } else if (message == S_TAG) {
                    Check(state.Read(r), "valid server state");
                    ++states;
                } else if (message == S_CHAT) {
                    r.U8();
                    r.U32();
                    auto ru = r.Str(), en = r.Str();
                    chat += en.empty() ? ru : en;
                } else if (message == S_PROFILE) {
                    auto bytes = r.Bytes();
                    profile.assign(bytes.begin(), bytes.end());
                    profileReceived = true;
                } else if (message == S_MONEY_APPLY || message == S_GEAR_APPLY)
                    ++transfers;
                else if (message == S_REJECT)
                    throw std::runtime_error("rejected by server");
                enet_packet_destroy(ev.packet);
            }
        }
    }
};
void Wait(Actor &a, Actor &b, const std::function<bool()> &pred, const char *why, unsigned timeout = 5000) {
    auto end = GetTickCount64() + timeout;
    while (GetTickCount64() < end) {
        a.Poll();
        b.Poll();
        if (pred())
            return;
        Sleep(2);
    }
    throw std::runtime_error(std::string("timeout: ") + why + "; " + a.chat + " " + b.chat);
}
void Barrier(Actor &a, Actor &b) {
    b.chat.clear();
    a.Chat("tag-barrier");
    Wait(a, b, [&] { return b.chat.find("tag-barrier") != std::string::npos; }, "network barrier");
}
void Integration(uint16_t port) {
    Actor a("Host"), b("Partner");
    a.Connect(port);
    Wait(a, b, [&] { return a.joined; }, "host");
    b.Connect(port);
    Wait(a, b, [&] { return b.joined; }, "partner");
    Barrier(a, b);
    auto phase = [&](uint8_t p) {
        Wait(a, b, [&] { return a.state.phase == p && b.state.phase == p; }, "both phases");
    };
    a.Action(tag::Start);
    phase(tag::Preparing);
    auto token = a.state.session;
    a.Action(tag::Ready);
    a.Action(tag::Ready);
    Barrier(a, b);
    Check(a.state.phase == tag::Preparing, "duplicate ready started round");
    b.Action(tag::Reject, 0, UINT32_MAX, {}, tag::Wanted);
    phase(tag::Restoring);
    Check(a.state.reason == tag::Wanted && a.state.actor == b.id && b.state.reason == tag::Wanted,
          "both players need cancellation reason and actor");
    a.Action(tag::Restored);
    b.Action(tag::Restored);
    phase(tag::Idle);
    a.Action(tag::Start);
    phase(tag::Preparing);
    Check(a.state.session != token, "new session token");
    b.Profile("original-before-tag");
    a.Action(tag::Ready);
    b.Action(tag::Ready);
    b.Profile("temporary-after-ready");
    phase(tag::Moving);
    a.Action(tag::Arrived);
    a.Action(tag::Arrived);
    Barrier(a, b);
    Check(a.state.phase == tag::Moving, "teleport barrier bypassed");
    b.Action(tag::Arrived);
    phase(tag::Hiding);
    Check(a.state.remaining <= 35000 && a.state.remaining > 33000, "hide deadline starts after both teleports");
    auto &catcher = a.state.catcher == a.id ? a : b;
    auto &runner = a.state.runner == a.id ? a : b;
    catcher.Action(tag::Hit);
    Barrier(a, b);
    Check(a.state.phase == tag::Hiding, "caught during hiding");
    b.Profile("temporary-during-round");
    a.Chat("/pay " + std::to_string(b.id) + " 1");
    Barrier(a, b);
    Check(a.transfers + b.transfers == 0, "money changed in temporary world");
    // Keep both clients alive while the actual server clock runs its 35 second countdown.
    auto next = GetTickCount64();
    Wait(
        a, b,
        [&] {
            if (GetTickCount64() >= next) {
                a.Action(tag::Pulse, 0, UINT32_MAX, {0, 0, 10});
                b.Action(tag::Pulse, 0, UINT32_MAX, {1, 0, 10});
                next = GetTickCount64() + 300;
            }
            return a.state.phase == tag::Chasing && b.state.phase == tag::Chasing;
        },
        "real hiding timer", 37000);
    runner.Action(tag::Hit);
    Barrier(a, b);
    Check(a.state.phase == tag::Chasing, "wrong player claimed catch");
    catcher.Action(tag::Hit, token);
    Barrier(a, b);
    Check(a.state.phase == tag::Chasing, "old session hit accepted");
    a.Action(tag::Pulse, 0, UINT32_MAX, {0, 0, 10});
    b.Action(tag::Pulse, 0, UINT32_MAX, {100, 0, 10});
    catcher.Action(tag::Hit);
    Barrier(a, b);
    Check(a.state.phase == tag::Chasing, "out of range catch accepted");
    a.Action(tag::Pulse, 0, UINT32_MAX, {0, 0, 10});
    b.Action(tag::Pulse, 0, UINT32_MAX, {1, 0, 10});
    Barrier(a, b);
    catcher.Action(tag::Hit);
    phase(tag::Result);
    Check(a.state.winner == catcher.id, "catch winner");
    auto old = a.state;
    next = GetTickCount64();
    Wait(
        a, b,
        [&] {
            if (GetTickCount64() >= next) {
                a.Action(tag::Pulse);
                b.Action(tag::Pulse);
                next = GetTickCount64() + 300;
            }
            return a.state.phase == tag::Moving && b.state.phase == tag::Moving;
        },
        "auto next round", 7000);
    Check(a.state.catcher == old.runner && a.state.runner == old.catcher && a.state.round == old.round + 1 &&
              a.state.location != old.location,
          "auto role/location change");
    a.Action(tag::End, 0, old.round);
    phase(tag::Restoring); // cancel remains valid across a round transition
    b.Profile("temporary-during-restoration");
    a.Action(tag::Restored);
    a.Action(tag::Restored);
    Barrier(a, b);
    Check(a.state.phase == tag::Restoring, "restore barrier bypassed");
    b.Action(tag::Restored);
    phase(tag::Idle);
    a.Action(tag::Start);
    phase(tag::Preparing);
    a.Action(tag::Ready);
    b.Action(tag::Ready);
    phase(tag::Moving);
    auto failed = a.state;
    a.Action(tag::Arrived);
    b.Action(tag::SpawnFailed, 0, UINT32_MAX, {}, tag::GroundMissing);
    Wait(a, b, [&] { return a.state.round > failed.round && a.state.round == b.state.round; }, "retry shared spawn");
    Check(a.state.phase == tag::Moving && a.state.location != failed.location && a.state.catcher == failed.catcher &&
              a.state.runner == failed.runner && a.state.reason == tag::GroundMissing && a.state.actor == b.id,
          "spawn retry state");
    a.Action(tag::Arrived, 0, failed.round);
    b.Action(tag::Arrived, 0, failed.round);
    Barrier(a, b);
    Check(a.state.phase == tag::Moving, "stale arrival ACKs started hiding after retry");
    a.Action(tag::Arrived);
    b.Action(tag::Arrived);
    phase(tag::Hiding);
    Check(a.state.reason == tag::None, "successful spawn kept stale failure reason");
    a.Action(tag::End);
    phase(tag::Restoring);
    a.Action(tag::Restored);
    b.Action(tag::Restored);
    phase(tag::Idle);
    a.Action(tag::Start);
    phase(tag::Preparing);
    a.Action(tag::Ready);
    b.Action(tag::Ready);
    phase(tag::Moving);
    for(unsigned attempt=0;attempt<4;++attempt) {
        auto generation=a.state.round;
        a.Action(tag::SpawnFailed,0,UINT32_MAX,{},tag::Water);
        Wait(a,b,[&]{return (a.state.round>generation && a.state.round==b.state.round) ||
             (a.state.phase==tag::Restoring && b.state.phase==tag::Restoring);},"bounded spawn retries");
        if(attempt<3) Check(a.state.phase==tag::Moving,"spawn retry budget exhausted early");
    }
    Check(a.state.phase==tag::Restoring && a.state.reason==tag::NoLocations,"exhausted retries must restore both players");
    a.Action(tag::Restored); b.Action(tag::Restored);phase(tag::Idle);
    a.Action(tag::Start);phase(tag::Preparing);a.Action(tag::Ready);b.Action(tag::Ready);phase(tag::Moving);
    b.Close();
    Wait(a, b, [&] { return a.state.phase == tag::Restoring; }, "disconnect restores survivor");
    a.Action(tag::Restored);
    Wait(a, b, [&] { return a.state.phase == tag::Idle; }, "disconnect barrier completes");
    b.Connect(port);
    Wait(a, b, [&] { return b.joined && b.profileReceived; }, "profile after reconnect");
    Check(b.profile == "original-before-tag", "temporary state replaced persistent profile");
    std::cout << "Tag ENet preparation, rejection, teleport barriers, real 35s timer, hit validation, role swap, "
                 "cancellation and disconnect tests passed\n";
}
int main(int argc, char **argv) {
    try {
        Unit();
        Check(enet_initialize() == 0, "enet init");
        if (argc > 1)
            Integration(uint16_t(std::stoi(argv[1])));
        enet_deinitialize();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
