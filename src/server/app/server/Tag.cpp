#include "app/server/Server.h"
#include "common/api/net/Net.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
#include <random>

namespace coop::server {
using namespace proto;
namespace {
std::mt19937 &Random() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
} // namespace
void Server::SendTag() {
    const auto &s = m_tag.state;
    if (s.session != m_tagLogged.session || s.phase != m_tagLogged.phase || s.round != m_tagLogged.round) {
        LOGI("Tag session %llu generation %u phase %u location %s catcher %u runner %u reason %s actor %u",
             (unsigned long long)s.session, s.round, s.phase, tag::locations[s.location].name, s.catcher, s.runner,
             tag::ReasonText(s.reason, true), s.actor);
        m_tagLogged = s;
    }
    Writer w(S_TAG);
    m_tag.Wire(NowMs()).Write(w);
    Broadcast(CH_RELIABLE, w);
    m_tagSent = NowMs();
}
void Server::EndTag(uint8_t reason, uint8_t actor) {
    if (!m_tag.Active() || m_tag.state.phase == tag::Restoring)
        return;
    m_tag.state.reason = reason;
    m_tag.state.actor = actor;
    m_tag.Set(tag::Restoring, NowMs(), 20000);
    SendTag();
}
void Server::HandleTag(Player &p, Reader &r) {
    uint8_t action = r.U8();
    uint64_t session = r.U64();
    uint32_t round = r.U32();
    Vec3 position;
    if (action == tag::Pulse)
        position = r.Vec();
    uint8_t reason = tag::Busy;
    if ((action == tag::Reject || action == tag::SpawnFailed) && r.Remaining())
        reason = r.U8();
    if (!r.Ok() || action > tag::SpawnFailed || reason > tag::PeerResynced)
        return;
    auto &s = m_tag.state;
    if (action == tag::Start) {
        if (m_tag.Active()) {
            SendTag();
            return;
        }
        bool safe = JoinedCount() == 2 && m_transfers.empty() && !m_worldWantedTime && m_worldWaiters.empty();
        uint8_t ids[2] = {255, 255};
        int n = 0;
        for (auto &other : m_players)
            if (other.joined) {
                if (other.status != STATUS_INGAME || other.staleWorld || other.follower || !other.upload.empty())
                    safe = false;
                if (n < 2)
                    ids[n++] = other.id;
            }
        if (!safe) {
            SendChat(&p, kServerId, 0xFF6464FF,
                     "Догонялки: нужны два игрока в общем мире, без загрузок и незавершённых передач.",
                     "Tag requires two players in the shared world, no loads or pending transfers.");
            return;
        }
        if (Random()() & 1)
            std::swap(ids[0], ids[1]);
        m_tag.Begin(++m_nextTransferId, ids[0], ids[1], NowMs());
        for (auto &other : m_players)
            if (other.joined) {
                m_tagInstance[other.id] = other.instanceId;
                m_tagPulse[other.id] = NowMs();
            }
        SendTag();
        return;
    }
    if (!m_tag.Active() || session != s.session || (p.id != s.catcher && p.id != s.runner) ||
        p.instanceId != m_tagInstance[p.id])
        return;
    if (action == tag::End || action == tag::Reject) {
        EndTag(action == tag::End ? tag::UserCancelled : reason, p.id);
        return;
    }
    if (round != s.round)
        return;
    if (action == tag::SpawnFailed) {
        if (s.phase != tag::Moving || (reason != tag::GroundMissing && reason != tag::GroundHeight &&
                                       reason != tag::Water && reason != tag::TeleportFailed))
            return;
        LOGW("Tag player %u rejected spawn %s: %s", p.id, tag::locations[s.location].name,
             tag::ReasonText(reason, true));
        if (m_tag.RetryLocation(NowMs(), Random()())) {
            s.reason = reason;
            s.actor = p.id;
            SendTag();
        } else
            EndTag(tag::NoLocations, p.id);
        return;
    }
    if (action == tag::Pulse) {
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
            EndTag(tag::TeleportFailed, p.id);
            return;
        }
        m_tagPosition[p.id] = position;
        m_tagPulse[p.id] = NowMs();
        return;
    }
    bool changed = false;
    if (action == tag::Ready && s.phase == tag::Preparing && m_tag.Ack(p.id)) {
        m_tag.Next(NowMs(), Random()());
        changed = true;
    } else if (action == tag::Arrived && s.phase == tag::Moving && m_tag.Ack(p.id)) {
        s.reason = tag::None;
        s.actor = 255;
        m_tag.Set(tag::Hiding, NowMs(), 35000);
        changed = true;
    } else if (action == tag::Restored && s.phase == tag::Restoring && m_tag.Ack(p.id)) {
        m_tag.Set(tag::Idle, NowMs(), 0);
        changed = true;
    } else if (action == tag::Hit && s.phase == tag::Chasing && p.id == s.catcher) {
        auto a = m_tagPosition[s.catcher], b = m_tagPosition[s.runner];
        if (NowMs() - m_tagPulse[s.catcher] < 2000 && NowMs() - m_tagPulse[s.runner] < 2000 &&
            tag::InReach(a.x, a.y, a.z, b.x, b.y, b.z))
            changed = m_tag.Catch(p.id, NowMs());
    }
    if (changed)
        SendTag();
}
void Server::ServiceTag() {
    if (!m_tag.Active())
        return;
    auto now = NowMs();
    auto &s = m_tag.state;
    for (auto id : {s.catcher, s.runner}) {
        auto *p = FindById(id);
        if (!p || p->instanceId != m_tagInstance[id] || p->status != STATUS_INGAME)
            EndTag(tag::PeerUnavailable, id);
        else if (now - m_tagPulse[id] > 10000)
            EndTag(tag::HeartbeatTimeout, id);
        if (s.phase == tag::Restoring && (!p || p->instanceId != m_tagInstance[id]))
            m_tag.Ack(id);
    }
    if (s.phase == tag::Restoring &&
        (m_tag.acknowledgements == ((1u << s.catcher) | (1u << s.runner)) || now >= m_tag.deadline)) {
        m_tag.Set(tag::Idle, now, 0);
        SendTag();
        return;
    }
    if (s.phase == tag::Result && now >= m_tag.deadline) {
        m_tag.Next(now, Random()());
        SendTag();
    } else if (m_tag.Tick(now) || now - m_tagSent >= 1000)
        SendTag();
}
} // namespace coop::server
