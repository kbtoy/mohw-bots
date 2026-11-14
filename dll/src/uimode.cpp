#include "uimode.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "engine.h"
#include "hook.h"
#include "log.h"

namespace uimode {
namespace {

constexpr uint32_t kHudGameModeKey = 0x674C5F75;   // what getgamemodegraph asks for (data key 1733092213)
constexpr uint32_t kCodeGameModeKey = 0x674CE375;  // what the provider answers with the level setup's "gamemode"

// The names the HUD graph's outputs compare against (the modes' own names, as in the level keys).
const char* const kModes[] = {"TeamDeathMatch", "SectorControl", "CombatMission", "BombSquad", "Sport", "CaptureTheFlag"};

using GetDataFn = bool(__thiscall*)(void* comp, uint32_t key, void* out);
GetDataFn g_getData = nullptr;
const char* volatile g_mode = nullptr;  // one of kModes, or null before a level
const char* g_logged = nullptr;          // UI thread: the mode last logged as answered

bool __fastcall GetDataDetour(void* comp, void* /*edx*/, uint32_t key, void* out)
{
    if (key == kHudGameModeKey || key == kCodeGameModeKey)
    {
        const char* mode = g_mode;
        if (mode && out && eng::UiSetString(out, mode))
        {
            if (mode != g_logged)
            {
                g_logged = mode;
                Log("ui: your game's HUD asked for the game mode; answered \"%s\"", mode);
            }
            return true;
        }
        key = kCodeGameModeKey;  // no level known yet: the game's own answer
    }
    return g_getData(comp, key, out);
}

}  // namespace

void Install()
{
    // The provider (UISettingsComp vtable 0x2827074 slot +0x28), thiscall(key, out) ret 8: sub esp, 0x94 (6 bytes).
    void* target = reinterpret_cast<void*>(eng::Address("UISettings_getData"));
    if (target && eng::Address("UIDataValue_setString"))
        g_getData = reinterpret_cast<GetDataFn>(InlineHook(target, 6, reinterpret_cast<const void*>(&GetDataDetour)));
    Log("ui: game mode for the HUD %s", g_getData ? "answered from the level (each mode gets its own HUD)"
                                                  : "hook FAILED (every mode shows the Team Deathmatch HUD)");
}

void OnLevel(const std::string& key)
{
    const std::string mode = key.size() > 6 ? key.substr(6) : std::string();
    const char* found = nullptr;
    for (const char* m : kModes)
        if (mode == m)
            found = m;
    g_mode = found;
}

}  // namespace uimode
