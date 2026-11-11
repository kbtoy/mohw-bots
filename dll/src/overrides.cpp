#include "overrides.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine.h"
#include "hardcore.h"
#include "log.h"

namespace {

float g_timeout = 0.0f;
float g_gameModeCounter = 0.0f;  // SyncedBFSettings.GameModeCounterModifier to force (0 = leave)
bool g_noIdleKick = false;       // BFServerSettings kicks of a human forced off (SetHumanKicks)
bool g_noTeamKillKick = false;
bool g_minimapSpotting = false;
volatile LONG g_presetLogged = 0;

// The preset flags that hide spotted enemies (or the whole minimap) on the HUD; forced off with
// [spotting] ForceMinimapSpotting. Logged once as found, so the log shows what the offline preset had.
struct ByteOverride
{
    uintptr_t typeInfo;
    int offset;
    const char* name;
};

const ByteOverride kMinimapFlags[] = {
    {eng::kTypeInfo_SyncedBFSettings, 0x10, "SyncedBFSettings.NoMinimap"},
    {eng::kTypeInfo_SyncedBFSettings, 0x12, "SyncedBFSettings.NoMinimapSpotting"},
    {eng::kTypeInfo_SyncedBFSettings, 0x13, "SyncedBFSettings.No3dSpotting"},
    {eng::kTypeInfo_SyncedMOHWSettings, 0x0C, "SyncedMOHWSettings.HudNoMinimap"},
};
char g_layerInclusion[256] = {};  // owned here for the game's lifetime: GameSettings keeps pointing at it
volatile LONG g_layerTableLogged = 0;

struct FloatOverride
{
    uintptr_t typeInfo;
    int offset;
    const char* name;
};

const FloatOverride kTimeouts[] = {
    {eng::kTypeInfo_ClientSettings, eng::kClientSettings_LoadingTimeout, "ClientSettings.LoadingTimeout"},
    {eng::kTypeInfo_ClientSettings, eng::kClientSettings_LoadedTimeout, "ClientSettings.LoadedTimeout"},
    {eng::kTypeInfo_ClientSettings, eng::kClientSettings_IngameTimeout, "ClientSettings.IngameTimeout"},
    {eng::kTypeInfo_ServerSettings, eng::kServerSettings_LoadingTimeout, "ServerSettings.LoadingTimeout"},
    {eng::kTypeInfo_ServerSettings, eng::kServerSettings_IngameTimeout, "ServerSettings.IngameTimeout"},
    {eng::kTypeInfo_NetworkSettings, eng::kNetworkSettings_ConnectTimeout, "NetworkSettings.ConnectTimeout"},
};

// Copies a NUL-terminated engine string without trusting the pointer.
void ReadCString(const void* source, char* out, size_t size)
{
    size_t i = 0;
    for (; source && i + 1 < size; ++i)
    {
        uint8_t c = 0;
        if (!eng::ProbeByte(static_cast<const char*>(source) + i, &c) || !c)
            break;
        out[i] = static_cast<char>(c);
    }
    out[i] = '\0';
}

// fb::Array<T>: pointer to the elements, element count in the dword before them.
uint32_t ArrayCount(const void* elements)
{
    void* count = nullptr;
    if (!elements || !eng::ProbePtr(static_cast<const char*>(elements) - 4, &count))
        return 0;
    uint32_t n = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(count));
    return n < 64 ? n : 0;
}

// Logs every criterion of GameSettings.LayerInclusionTable with its options, e.g. "GameMode: TeamDeathMatch, ...".
void LogLayerInclusionTable(const void* gameSettings)
{
    void* table = nullptr;
    void* criteria = nullptr;
    if (!eng::ProbePtr(eng::At(gameSettings, eng::kGameSettings_LayerInclusionTable), &table) || !table ||
        !eng::ProbePtr(eng::At(table, 0x0C), &criteria))
    {
        Log("overrides: GameSettings.LayerInclusionTable not set");
        return;
    }
    for (uint32_t i = 0, n = ArrayCount(criteria); i < n; ++i)
    {
        void* criterion = nullptr;
        void* name = nullptr;
        void* options = nullptr;
        if (!eng::ProbePtr(static_cast<char*>(criteria) + i * 4, &criterion) || !criterion)
            continue;
        eng::ProbePtr(eng::At(criterion, 0x08), &name);
        eng::ProbePtr(eng::At(criterion, 0x0C), &options);
        char line[512];
        ReadCString(name, line, 64);
        size_t used = std::strlen(line);
        used += std::snprintf(line + used, sizeof(line) - used, ":");
        for (uint32_t k = 0, m = ArrayCount(options); k < m && used + 40 < sizeof(line); ++k)
        {
            void* option = nullptr;
            char text[40];
            eng::ProbePtr(static_cast<char*>(options) + k * 4, &option);
            ReadCString(option, text, sizeof(text));
            used += std::snprintf(line + used, sizeof(line) - used, " %s", text);
        }
        Log("overrides: layer inclusion criterion %s", line);
    }
}

// GameSettings.DefaultLayerInclusion: logs the value the game has (once), then points it at ours if one is set.
void ApplyLayerInclusion()
{
    void* settings = nullptr;
    if (!eng::GetSettingsContainer(eng::kTypeInfo_GameSettings, &settings) || !settings)
        return;
    void* current = nullptr;
    eng::ProbePtr(eng::At(settings, eng::kGameSettings_DefaultLayerInclusion), &current);
    if (InterlockedExchange(&g_layerTableLogged, 1) == 0)
    {
        char text[256];
        ReadCString(current, text, sizeof(text));
        Log("overrides: GameSettings.DefaultLayerInclusion is \"%s\"", text);
        LogLayerInclusionTable(settings);
    }
    if (!g_layerInclusion[0] || current == g_layerInclusion)
        return;
    *reinterpret_cast<const char**>(static_cast<char*>(settings) + eng::kGameSettings_DefaultLayerInclusion) =
        g_layerInclusion;
    Log("overrides: GameSettings.DefaultLayerInclusion -> \"%s\"", g_layerInclusion);
}

}  // namespace

void InitOverrides(int timeoutSeconds, const char* layerInclusion, bool minimapSpotting)
{
    g_minimapSpotting = minimapSpotting;
    if (minimapSpotting)
        Log("overrides: the preset's NoMinimap / NoMinimapSpotting / No3dSpotting / HudNoMinimap will be forced off");
    g_timeout = timeoutSeconds > 0 ? static_cast<float>(timeoutSeconds) : 0.0f;
    if (g_timeout > 0.0f)
        Log("overrides: connection timeouts will be raised to %d s once the settings exist", timeoutSeconds);
    if (layerInclusion && layerInclusion[0])
    {
        strncpy_s(g_layerInclusion, layerInclusion, _TRUNCATE);
        Log("overrides: GameSettings.DefaultLayerInclusion will be set to \"%s\"", g_layerInclusion);
    }
}

void ApplyOverrides()
{
    // Called from every XInput poll (several times a frame) and every server tick. Once everything is in place,
    // only re-check every 2 s (in case a level load resets settings) instead of ~7 container lookups per call.
    static volatile LONG settled = 0;
    static volatile DWORD lastCheck = 0;
    DWORD now = GetTickCount();
    if (settled && now - lastCheck < 2000)
        return;
    lastCheck = now;

    bool allInPlace = true;
    ApplyLayerInclusion();
    if (g_layerInclusion[0])
    {
        void* settings = nullptr;
        void* current = nullptr;
        allInPlace &= eng::GetSettingsContainer(eng::kTypeInfo_GameSettings, &settings) && settings &&
                      eng::ProbePtr(eng::At(settings, eng::kGameSettings_DefaultLayerInclusion), &current) &&
                      current == g_layerInclusion;
    }
    // The preset's minimap flags: log what they were once, then force them off.
    {
        char found[256] = "";
        size_t used = 0;
        bool all = true;
        for (const ByteOverride& o : kMinimapFlags)
        {
            void* container = nullptr;
            uint8_t value = 0;
            if (!eng::GetSettingsContainer(o.typeInfo, &container) || !container ||
                !eng::ReadByte(eng::At(container, o.offset), &value))
            {
                all = false;
                continue;
            }
            used += std::snprintf(found + used, sizeof(found) - used, "%s%s=%u", used ? ", " : "",
                                  std::strchr(o.name, '.') + 1, value);
            if (g_minimapSpotting && value != 0 && !hardcore::Owns(o.typeInfo, o.offset))
            {
                *(static_cast<uint8_t*>(container) + o.offset) = 0;
                Log("overrides: %s %u -> 0", o.name, value);
            }
        }
        if (all && InterlockedExchange(&g_presetLogged, 1) == 0)
            Log("overrides: game preset minimap flags as found: %s", found);
        allInPlace &= all;
    }
    allInPlace &= hardcore::Apply();
    // The game-mode counter (BF3's vars.gameModeCounter): ServerKillCounterEntity (0xEF1DD0) and the ticket counters
    // (0xED4BC0) multiply the mode's target by it as they start - TDM's 75 kills per team - and the server sends it to
    // clients in NetworkSettingsMessage (+0x54), so the HUD shows the same target.
    if (g_gameModeCounter > 0.0f)
    {
        void* synced = nullptr;
        float current = 0.0f;
        if (eng::GetSettingsContainer(eng::kTypeInfo_SyncedBFSettings, &synced) && synced &&
            eng::ReadFloat(eng::At(synced, 0x0C), &current))
        {
            if (current != g_gameModeCounter)
            {
                *reinterpret_cast<float*>(static_cast<char*>(synced) + 0x0C) = g_gameModeCounter;
                Log("overrides: SyncedBFSettings.GameModeCounterModifier %.2f -> %.2f (TDM: %d kills to win)", current,
                    g_gameModeCounter, static_cast<int>(75.0f * g_gameModeCounter + 0.5f));
            }
        }
        else
            allInPlace = false;
    }
    // The server's kicks of its human (read live by the checks, so a write here is enough).
    if (g_noIdleKick || g_noTeamKillKick)
    {
        void* server = nullptr;
        if (eng::GetSettingsContainer(eng::kTypeInfo_BFServerSettings, &server) && server)
        {
            auto zero = [server](int offset, const char* name, bool asFloat) {
                uint32_t raw = 0;
                if (!eng::ReadU32(eng::At(server, offset), &raw) || raw == 0)
                    return;
                float asF = 0.0f;
                std::memcpy(&asF, &raw, sizeof(asF));
                *reinterpret_cast<uint32_t*>(static_cast<char*>(server) + offset) = 0;  // 0 and 0.0f alike
                if (asFloat)
                    Log("overrides: BFServerSettings.%s %.1f -> 0 (that kick is off)", name, asF);
                else
                    Log("overrides: BFServerSettings.%s %u -> 0 (that kick is off)", name, raw);
            };
            if (g_noIdleKick)
                zero(eng::kBFServerSettings_IdleTimeout, "NoInteractivityTimeoutTime", true);
            if (g_noTeamKillKick)
            {
                zero(eng::kBFServerSettings_TeamKillCountForKick, "TeamKillCountForKick", false);
                zero(eng::kBFServerSettings_TeamKillValueForKick, "TeamKillValueForKick", true);
            }
        }
        else
            allInPlace = false;
    }
    if (g_timeout > 0.0f)
        for (const FloatOverride& o : kTimeouts)
        {
            void* container = nullptr;
            float current = 0.0f;
            if (!eng::GetSettingsContainer(o.typeInfo, &container) || !container ||
                !eng::ReadFloat(eng::At(container, o.offset), &current))
            {
                allInPlace = false;  // container not created yet
                continue;
            }
            if (current == g_timeout)
                continue;
            *reinterpret_cast<float*>(static_cast<char*>(container) + o.offset) = g_timeout;
            Log("overrides: %s %.1f -> %.1f", o.name, current, g_timeout);
        }
    settled = allInPlace ? 1 : 0;
}

namespace {
char g_launchMap[32] = "";
volatile LONG g_mapNameLogged = 0;
}  // namespace

void SetLaunchMap(const char* code)
{
    strncpy_s(g_launchMap, code ? code : "", _TRUNCATE);
    InterlockedExchange(&g_mapNameLogged, 0);  // an in-place level change: name the new map once it's in
}

void ChangeLayerInclusion(const char* layerInclusion)
{
    // GameSettings keeps pointing at g_layerInclusion (or is pointed at it now), so the new text is what the game reads.
    strncpy_s(g_layerInclusion, layerInclusion ? layerInclusion : "", _TRUNCATE);
    ApplyLayerInclusion();
    Log("overrides: GameSettings.DefaultLayerInclusion now \"%s\"", g_layerInclusion);
}

void ClientMapNameTick()
{
    if (!g_launchMap[0] || g_mapNameLogged)
        return;
    char sid[64];
    char name[128];
    std::snprintf(sid, sizeof(sid), "ID_M_LEVEL_NAME_%s", g_launchMap);
    if (InterlockedExchange(&g_mapNameLogged, 1))
        return;  // once: a fault would repeat
    if (eng::Localize(sid, name, sizeof(name)))
        Log("map: %s = \"%s\"", g_launchMap, name);
}

void SetGameModeCounter(int percent)
{
    if (percent <= 0)
        return;
    g_gameModeCounter = percent / 100.0f;
    Log("overrides: game mode counter %d%% (Team Deathmatch: %d kills to win; the 10-minute timer still ends a round)",
        percent, static_cast<int>(75.0f * g_gameModeCounter + 0.5f));
}

void SetHumanKicks(bool idleKick, bool teamKillKick)
{
    g_noIdleKick = !idleKick;
    g_noTeamKillKick = !teamKillKick;
    Log("overrides: you can be kicked for being idle: %s; for team kills: %s (a kick ends an offline match)",
        idleKick ? "yes" : "no", teamKillKick ? "yes" : "no");
}
