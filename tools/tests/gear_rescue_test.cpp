#include <iostream>
#include <stdexcept>
#include "common/lib/gear/Gear.h"
#include "common/lib/rescue/Rescue.h"

using namespace coop;
static void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

int main() {
    try {
        using namespace gear;
        Check(Slot(22) == 2 && Slot(23) == 2 && Slot(30) == 5 && Slot(32) == 4 && Slot(4) == 1, "weapon slots");
        for (int type : {0, 19, 20, 21, 39, 40, 44, 45, 46, 255}) Check(Slot(type) == -1, "special equipment accepted");
        Check(!Item{Ammo, 4, 1}.Valid() && !Item{Ammo, 22, 0}.Valid() && !Item{Gun, 22, -1}.Valid(), "invalid item");
        Receipts r;
        SlotState a{22, 500, 17}, b{22, 100, 7};
        Item ammo{Ammo, 22, 50};
        Check(r.Apply(1, Debit, ammo, true, true, a) == Ok && a.ammo == 450 && a.clip == 17, "ammo debit");
        Check(r.Apply(1, Debit, ammo, true, true, a) == Ok && a.ammo == 450, "duplicate debit");
        Check(r.Apply(2, Credit, ammo, true, true, b) == Ok && b.ammo == 150 && b.clip == 7, "ammo credit must not reload");
        Check(r.Apply(2, Credit, ammo, false, false, b) == Ok && b.ammo == 150, "duplicate credit while loading");
        Item gun{Gun, 22, 450};
        Check(r.Apply(3, Debit, gun, true, true, a) == Ok && a.weapon == 0, "full gun debit");
        Check(r.Apply(3, Refund, gun, true, false, a) == Ok && a.weapon == 22 && a.ammo == 450 && a.clip == 17, "refund weapon and old clip");
        Check(r.Apply(3, Refund, gun, true, false, a) == Ok && a.ammo == 450, "duplicate refund");
        auto before = a;
        Check(r.Apply(4, Debit, Item{Gun, 22, 400}, true, true, a) == NotEnough && a == before, "full weapon snapshot changed");
        Check(r.Apply(5, Debit, Item{Ammo, 22, 451}, true, true, a) == NotEnough && a == before, "insufficient rounds");
        Check(r.Apply(6, Debit, ammo, true, false, a) == TooFar && a == before, "range check");
        Check(r.Apply(7, Debit, ammo, false, true, a) == Unavailable && a == before, "AFK/downed/loading sender");
        SlotState occupied{23, 50, 10}, old = occupied;
        Check(r.Apply(8, Credit, gun, true, true, occupied) == SlotOccupied && occupied == old, "different weapon destroyed");
        SlotState empty;
        Check(r.Apply(9, Credit, ammo, true, true, empty) == WrongWeapon && empty.weapon == 0, "ammo created missing gun");
        SlotState full{22, kMaxAmmo, 17};
        Check(r.Apply(10, Credit, ammo, true, true, full) == Overflow && full.ammo == kMaxAmmo, "ammo overflow");
        Check(r.Apply(11, Debit, ammo, true, true, a) == Ok, "refund setup");
        a.weapon = 23;
        Check(r.Apply(11, Refund, ammo, true, false, a) == Busy, "refund overwrote occupied slot");
        a.weapon = 22;
        Check(r.Apply(11, Refund, ammo, true, false, a) == Ok && a.ammo == before.ammo, "retry deferred refund");
        SlotState zero{22, 0, 0};
        Check(r.Apply(12, Debit, Item{Gun, 22, 0}, true, true, zero) == Ok && zero.weapon == 0, "empty gun transfer");
        SlotState melee{4, 1, 1};
        Check(r.Apply(13, Debit, Item{Gun, 4, 1}, true, true, melee) == Ok && melee.weapon == 0, "melee transfer");

        rescue::Cycle cycle;
        Check(!cycle.Help(1, 0), "revive healthy player");
        cycle.Begin(100, 1000);
        Check(cycle.Get().Active() && cycle.Get().seconds == 60, "downed start");
        Check(cycle.Help(1, 2000) && !cycle.Help(0, 2100), "multiple rescues");
        Check(cycle.Advance(4999, true).Active(), "revived too early");
        auto saved = cycle.Advance(5000, true);
        Check(saved.stage == rescue::Revived && !saved.Active() && saved.helper == 255 && saved.seconds == 0, "three second revive");
        cycle.Begin(101, 10000); cycle.Help(1, 11000);
        Check(cycle.Advance(12000, false).helper == 255 && cycle.Get().Active(), "interrupted rescue");
        Check(cycle.Advance(69999, true).Active(), "bled out too early");
        Check(cycle.Advance(70000, true).stage == rescue::Expired, "bleed out deadline");
        cycle.Begin(102, 0); cycle.Help(1, 59000);
        Check(cycle.Advance(60000, true).stage == rescue::Expired, "rescue after deadline");
        cycle.Begin(103, 0); cycle.Help(1, 500); cycle.Clear();
        Check(!cycle.Get().Active() && cycle.Get().stage == rescue::Normal, "session cleanup");
        for (auto state : {rescue::State{}, rescue::State{1, rescue::Downed, 60, 255}, rescue::State{2, rescue::Revived, 0, 255}, rescue::State{3, rescue::Expired, 0, 255}}) {
            Writer w; state.Write(w); Reader rd(w.Data(), w.Size()); rescue::State out;
            Check(out.Read(rd) && out.stage == state.stage && out.episode == state.episode, "injury wire roundtrip");
        }
        for (auto state : {rescue::State{0, rescue::Downed, 30, 255}, rescue::State{1, rescue::Downed, 61, 255}, rescue::State{1, rescue::Revived, 5, 255}, rescue::State{1, rescue::Downed, 20, 9}}) {
            Writer w; state.Write(w); Reader rd(w.Data(), w.Size()); rescue::State out;
            Check(!out.Read(rd), "malformed injury accepted");
        }
        std::cout << "Equipment inventory, replay/refund and revive-state/timer tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
