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

// UI thread. 2026.11.13.3 answered "SectorControl" on MP_18 and the Sector Control HUD still didn't show, so every key
// asked of this component is logged (the first 60 distinct ones, with whether the game handled it), and every game
// mode answer (which key, which component), to see whether the HUD's own key ever arrives.
uint32_t g_seenKeys[60];
int g_seenCount = 0;
int g_modeLogs = 0;

bool __fastcall GetDataDetour(void* comp, void* /*edx*/, uint32_t key, void* out)
{
    if (key == kHudGameModeKey || key == kCodeGameModeKey)
    {
        const char* mode = g_mode;
        if (mode && out && eng::UiSetString(out, mode))
        {
            if (g_modeLogs < 40)
            {
                ++g_modeLogs;
                Log("ui: game mode asked by key %08X (%s) of %p; answered \"%s\"", key,
                    key == kHudGameModeKey ? "the HUD graph's" : "the game's own", comp, mode);
            }
            return true;
        }
        key = kCodeGameModeKey;  // no level known yet: the game's own answer
    }
    const bool handled = g_getData(comp, key, out);
    bool seen = false;
    for (int i = 0; i < g_seenCount && !seen; ++i)
        seen = g_seenKeys[i] == key;
    if (!seen && g_seenCount < 60)
    {
        g_seenKeys[g_seenCount++] = key;
        Log("ui: settings data key %08X asked of %p: %s", key, comp, handled ? "answered by the game" : "NOT handled");
    }
    return handled;
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
