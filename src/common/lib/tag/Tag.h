#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "common/lib/buffer/Buffer.h"

namespace coop::tag {
enum Phase : uint8_t { Idle, Preparing, Moving, Hiding, Chasing, Result, Restoring };
enum Action : uint8_t { Start, End, Ready, Arrived, Hit, Restored, Reject, Pulse, SpawnFailed };
enum Reason : uint8_t {
    None,
    ClientNotReady,
    HooksUnavailable,
    WorldNotReady,
    ProfileNotReady,
    Mission,
    WorldChanging,
    MissionMarker,
    Dead,
    Interior,
    Wanted,
    Vehicle,
    Airborne,
    Busy,
    ControlLock,
    Scene,
    ScriptedTask,
    Afk,
    Injured,
    PlayerChanged,
    ConnectionLost,
    GroundMissing,
    GroundHeight,
    Water,
    TeleportFailed,
    PeerUnavailable,
    HeartbeatTimeout,
    PrepareTimeout,
    MoveTimeout,
    UserCancelled,
    NoLocations,
    WorldShutdown,
    HostChanged,
    PeerResynced
};
inline const char *ReasonText(uint8_t reason, bool english) {
    struct Text {
        const char *ru;
        const char *en;
    };
    static constexpr Text messages[] = {
        {"", ""},
        {"игра ещё не готова", "game is not ready"},
        {"не установились защитные перехваты игры", "game safety hooks are unavailable"},
        {"общий мир ещё не загружен", "shared world is not ready"},
        {"профиль ещё не загружен", "profile is not ready"},
        {"идёт сюжетная миссия", "a story mission is running"},
        {"ожидается загрузка мира", "world reload is pending"},
        {"отойдите от маркера миссии", "step away from the mission marker"},
        {"игрок погиб", "player is dead"},
        {"выйдите из интерьера на улицу", "leave the interior and stand outdoors"},
        {"сначала уберите розыск", "clear the wanted level first"},
        {"выйдите из транспорта", "leave the vehicle"},
        {"встаньте на землю и остановитесь", "stand still on the ground"},
        {"игрок занят другим действием", "player is busy with another activity"},
        {"игровое управление заблокировано", "game controls are locked"},
        {"идёт игровая сцена", "a game scene is running"},
        {"завершите текущее действие", "finish the current activity"},
        {"сначала выключите AFK", "disable AFK first"},
        {"игрок ранен", "player is wounded"},
        {"персонаж игрока изменился", "player character changed"},
        {"потерян ответ сервера", "server stopped responding"},
        {"земля в выбранной точке не загрузилась", "ground did not load at the selected point"},
        {"небезопасная высота площадки", "unsafe spawn height"},
        {"выбранная точка оказалась в воде", "selected point is in water"},
        {"игра не выполнила телепорт", "game did not complete the teleport"},
        {"напарник вышел или загружает игру", "partner left or is loading"},
        {"напарник перестал отвечать", "partner stopped responding"},
        {"не получено подтверждение подготовки", "preparation acknowledgement timed out"},
        {"не получено подтверждение телепорта", "teleport acknowledgement timed out"},
        {"режим остановлен командой /endgames", "mode stopped with /endgames"},
        {"не найдена безопасная площадка после повторных попыток", "no safe location found after retries"},
        {"игра загружает другой мир", "game is loading another world"},
        {"сменился хост", "host changed"},
        {"напарник перезагрузил мир", "partner reloaded the world"}};
    if (reason >= sizeof(messages) / sizeof(*messages))
        return english ? "unknown reason" : "неизвестная причина";
    return english ? messages[reason].en : messages[reason].ru;
}
struct Location {
    float x, y, z;
    const char *name;
};
// Outdoor pedestrian areas. The client MUST find loaded ground at both spawn points.
inline constexpr Location locations[] = {
    {2490, -1670, 14, "Grove Street"},   {1480, -1720, 14, "Pershing Square"}, {375, -2020, 8, "Santa Maria Beach"},
    {1950, -1200, 20, "Glen Park"},      {-1980, 135, 28, "Doherty"},          {-2650, 610, 15, "Palisades"},
    {-2020, 1080, 56, "Calton Heights"}, {2020, 1340, 11, "The Strip"},        {2500, 1530, 11, "Pilgrim"},
    {1180, 2730, 11, "Prickle Pine"},    {1280, 280, 20, "Montgomery"},        {2300, -80, 27, "Palomino Creek"},
    {-2200, -2300, 31, "Angel Pine"},    {-150, 1120, 20, "Fort Carson"},      {-840, 1450, 15, "Las Barrancas"},
    {-2450, 2300, 5, "Bayside"}};
inline constexpr unsigned locationCount = sizeof(locations) / sizeof(*locations);
struct State {
    uint64_t session = 0;
    uint32_t round = 0, remaining = 0;
    uint8_t phase = Idle, catcher = 255, runner = 255, location = 0, winner = 255;
    uint8_t reason = None, actor = 255;
    void Write(Writer &w) const {
        w.U64(session);
        w.U32(round);
        w.U8(phase);
        w.U8(catcher);
        w.U8(runner);
        w.U8(location);
        w.U8(winner);
        w.U32(remaining);
        w.U8(reason);
        w.U8(actor);
    }
    bool Read(Reader &r) {
        session = r.U64();
        round = r.U32();
        phase = r.U8();
        catcher = r.U8();
        runner = r.U8();
        location = r.U8();
        winner = r.U8();
        remaining = r.U32();
        reason = r.U8();
        actor = r.U8();
        return r.Ok() && phase <= Restoring && location < locationCount && remaining <= 600000 &&
               reason <= PeerResynced && (actor == 255 || actor < 4);
    }
};
// Server-owned clock. No client timestamps or frame counters determine winners.
struct Match {
    State state;
    uint64_t deadline = 0;
    uint8_t acknowledgements = 0;
    uint32_t failedLocations = 0;
    unsigned spawnAttempts = 0;
    bool Active() const { return state.phase != Idle; }
    void Set(uint8_t phase, uint64_t now, uint32_t duration) {
        state.phase = phase;
        deadline = now + duration;
        acknowledgements = 0;
    }
    void Begin(uint64_t token, uint8_t a, uint8_t b, uint64_t now) {
        state = {};
        failedLocations = 0;
        spawnAttempts = 0;
        state.session = token;
        state.catcher = a;
        state.runner = b;
        Set(Preparing, now, 15000);
    }
    bool Ack(uint8_t player) {
        if (player != state.catcher && player != state.runner)
            return false;
        acknowledgements |= uint8_t(1u << player);
        return acknowledgements == ((1u << state.catcher) | (1u << state.runner));
    }
    void Next(uint64_t now, unsigned random) {
        if (state.round)
            std::swap(state.catcher, state.runner);
        auto excluded = failedLocations | (state.round ? 1u << state.location : 0);
        if (!PickLocation(random, excluded))
            PickLocation(random, failedLocations);
        spawnAttempts = 1;
        ++state.round;
        state.winner = 255;
        state.reason = None;
        state.actor = 255;
        Set(Moving, now, 20000);
    }
    bool PickLocation(unsigned random, uint32_t excluded) {
        unsigned count = 0;
        for (unsigned i = 0; i < locationCount; ++i)
            if (!(excluded & (1u << i)))
                ++count;
        if (!count)
            return false;
        unsigned pick = random % count;
        for (unsigned i = 0; i < locationCount; ++i)
            if (!(excluded & (1u << i)) && pick-- == 0) {
                state.location = uint8_t(i);
                return true;
            }
        return false;
    }
    bool RetryLocation(uint64_t now, unsigned random) {
        if (state.phase != Moving)
            return false;
        failedLocations |= 1u << state.location;
        if (spawnAttempts >= 4 || !PickLocation(random, failedLocations))
            return false;
        ++spawnAttempts;
        ++state.round; // invalidate late arrival ACKs without changing either role
        Set(Moving, now, 20000);
        return true;
    }
    bool Catch(uint8_t by, uint64_t now) {
        if (state.phase != Chasing || by != state.catcher || now >= deadline)
            return false;
        state.winner = state.catcher;
        Set(Result, now, 5000);
        return true;
    }
    bool Tick(uint64_t now) {
        if (!Active() || now < deadline)
            return false;
        if (state.phase == Hiding)
            Set(Chasing, now, 600000);
        else if (state.phase == Chasing) {
            state.winner = state.runner;
            Set(Result, now, 5000);
        } else if (state.phase != Result && state.phase != Restoring) {
            state.reason = state.phase == Moving ? MoveTimeout : PrepareTimeout;
            state.actor = 255;
            Set(Restoring, now, 20000);
        } else
            return false;
        return true;
    }
    State Wire(uint64_t now) const {
        auto s = state;
        s.remaining = uint32_t(deadline > now ? deadline - now : 0);
        return s;
    }
};
inline bool InReach(float ax, float ay, float az, float bx, float by, float bz) {
    float x = ax - bx, y = ay - by, z = az - bz;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && x * x + y * y + z * z <= 25.0f;
}
} // namespace coop::tag
