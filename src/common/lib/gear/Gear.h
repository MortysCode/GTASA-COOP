#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>

namespace coop::gear {
constexpr int32_t kMaxAmmo = 99999;
enum Kind : uint8_t { Gun = 1, Ammo = 2 };
enum Action : uint8_t { Debit, Credit, Refund };
// First four values deliberately match the money protocol.
enum Result : uint8_t { Ok, Rejected, Busy, Unavailable, TooFar, SlotOccupied, WrongWeapon, NotEnough, Overflow };

inline int Slot(int type) {
    if (type == 1) return 0;
    if (type >= 2 && type <= 9) return 1;
    if (type >= 10 && type <= 15) return 10;
    if (type >= 16 && type <= 18) return 8;
    if (type >= 22 && type <= 24) return 2;
    if (type >= 25 && type <= 27) return 3;
    if (type == 28 || type == 29 || type == 32) return 4;
    if (type == 30 || type == 31) return 5;
    if (type == 33 || type == 34) return 6;
    if (type >= 35 && type <= 38) return 7;
    if (type >= 41 && type <= 43) return 9;
    return -1;  // fists, projectiles, satchels/detonators and special equipment aren't transferable
}
inline bool UsesAmmo(int type) { return (type >= 16 && type <= 18) || (type >= 22 && type <= 38) || (type >= 41 && type <= 43); }
struct Item {
    uint8_t kind = Gun, weapon = 0;
    int32_t ammo = 0;
    bool Valid() const {
        return (kind == Gun || kind == Ammo) && Slot(weapon) >= 0 && ammo >= 0 && ammo <= kMaxAmmo &&
               (kind != Ammo || (UsesAmmo(weapon) && ammo > 0));
    }
};
struct SlotState {
    int32_t weapon = 0, ammo = 0, clip = 0;
    bool operator==(const SlotState&) const = default;
};

class Receipts {
public:
    uint8_t Apply(uint64_t id, uint8_t action, Item item, bool ready, bool nearby, SlotState& slot) {
        auto key = std::make_pair(id, action);
        if (auto it = m_results.find(key); it != m_results.end()) return it->second;
        if (!id || action > Refund || !item.Valid()) return Rejected;
        auto finish = [&](uint8_t result) { if (result != Busy) m_results[key] = result; return result; };
        if (!ready) return finish(action == Refund ? Busy : Unavailable);
        if (action != Refund && !nearby) return finish(TooFar);
        if (action == Debit) {
            if (slot.weapon != item.weapon) return finish(WrongWeapon);
            if (slot.ammo < item.ammo || (item.kind == Gun && slot.ammo != item.ammo)) return finish(NotEnough);
            m_debits[id] = slot;
            if (item.kind == Gun) slot = {};
            else { slot.ammo -= item.ammo; slot.clip = std::min(slot.clip, slot.ammo); }
            return finish(Ok);
        }
        if (slot.weapon != 0 && slot.weapon != item.weapon)
            return finish(action == Refund ? Busy : SlotOccupied);
        if (item.kind == Ammo && action == Credit && slot.weapon != item.weapon) return finish(WrongWeapon);
        bool empty = slot.weapon == 0;
        if (int64_t(empty ? 0 : slot.ammo) + item.ammo > kMaxAmmo) return finish(action == Refund ? Busy : Overflow);
        if (action == Refund && !m_debits.count(id)) return finish(Rejected);
        if (empty) slot.ammo = slot.clip = 0;
        slot.weapon = item.weapon;
        slot.ammo += item.ammo;
        // Only restore the old clip into an empty slot; receiving ammo does not reload a gun.
        if (empty) slot.clip = action == Refund ? std::min(m_debits[id].clip, slot.ammo) : 0;
        return finish(Ok);
    }
private:
    std::map<std::pair<uint64_t, uint8_t>, uint8_t> m_results;
    std::map<uint64_t, SlotState> m_debits;
};
}  // namespace coop::gear
