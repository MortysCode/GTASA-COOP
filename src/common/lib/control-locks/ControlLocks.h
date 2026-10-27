#pragma once
#include <cstdint>

namespace coop::controls {
constexpr uint16_t kAfk = 0x02, kAid = 0x10, kTag = 0x200, kOwnedBits = kAfk | kAid | kTag;

// Only our own locks can be lifted for the camera. Game/mission/phone flags
// remain intact; unlike SET_PLAYER_CONTROL, neither custom bit is a scene flag.
class Locks {
public:
    uint16_t Mask() const { return m_mask; }
    void Acquire(uint16_t owner, uint16_t& padFlags) {
        owner &= kOwnedBits;
        m_mask |= owner; padFlags |= owner;
    }
    void Release(uint16_t owner, uint16_t& padFlags) {
        owner &= m_mask;
        padFlags &= ~owner; m_mask &= ~owner;
    }
    void Clear(uint16_t& padFlags) { Release(m_mask, padFlags); }
private:
    uint16_t m_mask = 0;
};

class CameraScope {
public:
    CameraScope(uint16_t& flags, const Locks& locks) : m_flags(flags), m_locks(locks), m_restore(flags & locks.Mask() & (kAfk | kAid)) {
        m_flags &= ~m_restore;
    }
    ~CameraScope() {
        // Do not re-lock a feature released during camera processing. Preserve
        // every unrelated flag set by the engine or another plugin meanwhile.
        m_flags |= m_restore & m_locks.Mask();
    }
    CameraScope(const CameraScope&) = delete;
    CameraScope& operator=(const CameraScope&) = delete;
private:
    uint16_t& m_flags;
    const Locks& m_locks;
    uint16_t m_restore;
};
}  // namespace coop::controls
