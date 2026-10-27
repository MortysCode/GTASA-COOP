#include "entities/player/ControlLocks.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/input/Input.h"

namespace coop {
namespace {
controls::Locks g_locks;
using CameraProcessFn = void(__fastcall*)(void*, void*);
CameraProcessFn g_origCameraProcess = nullptr;

void __fastcall HookCameraProcess(void* self, void* edx) {
    if (!g_locks.Mask() || !Net().Online() || !core::IsPlayerReady() || core::IsRestarting() ||
        game::MenuActive() || core::ConnectMenuVisible() || ui::In().KeyboardCaptured() || ui::In().MouseCaptured() ||
        game::cam::WideScreenOn() || game::Ref<uint8_t>(0xB5F851)) {
        g_origCameraProcess(self, edx);
        return;
    }
    // Camera input getters reject any DisablePlayerControls flag. Lift only
    // our flags for this call, never for ped simulation, tasks or weapon input.
    controls::CameraScope camera(game::GetPad(0)->DisablePlayerControls, g_locks);
    g_origCameraProcess(self, edx);
}

class PlayerControls : public Module {
public:
    const char* Name() const override { return "PlayerControls"; }
    void OnInstallHooks() override {
        mem::HookFn(0x52B730, &HookCameraProcess, &g_origCameraProcess, "CCamera::Process (AFK/injury free look)");
    }
    void OnSessionEnd() override { g_locks.Clear(game::GetPad(0)->DisablePlayerControls); }
    void OnGameShutdown() override { g_locks.Clear(game::GetPad(0)->DisablePlayerControls); }
};
PlayerControls g_controls;
struct Reg { Reg() { core::RegisterModule(&g_controls); } } g_reg;
}  // namespace

void AcquirePlayerControlLock(uint16_t owner) { g_locks.Acquire(owner, game::GetPad(0)->DisablePlayerControls); }
void ReleasePlayerControlLock(uint16_t owner) { g_locks.Release(owner, game::GetPad(0)->DisablePlayerControls); }
}  // namespace coop
