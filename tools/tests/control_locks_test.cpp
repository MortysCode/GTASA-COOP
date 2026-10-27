#include <iostream>
#include <stdexcept>
#include "common/lib/control-locks/ControlLocks.h"

using namespace coop::controls;
static void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

int main() {
    try {
        Locks locks;
        uint16_t tagFlags = 0;
        locks.Acquire(kTag, tagFlags);
        locks.Acquire(kAfk, tagFlags);
        { CameraScope camera(tagFlags, locks); Check(tagFlags == kTag, "tag catcher must remain blind and frozen"); }
        locks.Release(kTag, tagFlags);
        Check(tagFlags == kAfk, "tag cleanup removed AFK lock");
        locks.Clear(tagFlags);
        uint16_t flags = 0x20;  // actual mission/script lock
        { CameraScope camera(flags, locks); Check(flags == 0x20, "unowned mission lock lifted"); }
        locks.Acquire(kAfk, flags);
        Check(flags == (0x20 | kAfk), "AFK must block body input");
        {
            CameraScope camera(flags, locks);
            Check(flags == 0x20, "mission lock must still block camera");
            flags |= 0x80;  // game adds another scene restriction
        }
        Check(flags == (0x20 | 0x80 | kAfk), "camera restore lost a game flag");
        locks.Release(kAfk, flags);
        Check(flags == (0x20 | 0x80) && !locks.Mask(), "AFK cleanup lost game flags");

        flags = 0;
        locks.Acquire(kAid, flags);
        {
            CameraScope camera(flags, locks);
            Check(!flags, "wounded camera input is still locked");
            locks.Release(kAid, flags);  // revived during the scope
        }
        Check(!flags && !locks.Mask(), "camera re-locked a revived player");

        locks.Acquire(kAfk, flags);
        locks.Acquire(kAid, flags);
        {
            CameraScope camera(flags, locks);
            Check(!flags, "custom locks did not lift for camera");
            { CameraScope nested(flags, locks); Check(!flags, "nested camera call"); }
            Check(!flags, "nested camera restored too early");
            locks.Release(kAfk, flags);
        }
        Check(flags == kAid, "incorrect owner restored after nested camera call");
        locks.Clear(flags);
        Check(!flags && !locks.Mask(), "load/disconnect cleanup");

        flags = kAid;  // an unrelated plugin has set this bit; we don't own it
        locks.Acquire(kAfk, flags);
        { CameraScope camera(flags, locks); Check(flags == kAid, "unowned custom bit lifted"); }
        locks.Clear(flags);
        Check(flags == kAid, "cleanup removed an unowned flag");

        flags = 0;
        locks.Acquire(kAfk, flags);
        {
            CameraScope camera(flags, locks);
            locks.Acquire(kAid, flags); // a new owner appears while the camera is running
        }
        Check(flags == (kAfk | kAid), "newly acquired body lock was lost");
        locks.Clear(flags);
        locks.Acquire(kAfk, flags);
        try { CameraScope camera(flags, locks); throw 1; } catch (int) {}
        Check(flags == kAfk, "camera scope leaked unlocked controls");
        locks.Clear(flags);
        std::cout << "Camera lock ownership, restoration, cleanup and nested-scope tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
