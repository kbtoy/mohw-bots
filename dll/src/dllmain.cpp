#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "version.h"
#include "bots.h"
#include "engine.h"
#include "guards.h"
#include "hook.h"
#include "log.h"
#include "modes.h"
#include "overrides.h"
#include "relaunch.h"
#include "trace.h"

namespace {

// [launch] Map: one entry ("MP_05", "MP_03:SectorControl") or several that take turns, one per launch - the next one
// is remembered in mohw-bots-state.ini. Writes the chosen map code and mode (empty map: no Map set).
// ProCon's Restart Round leaves [launch] Once=<entry> in the state file: that plays once, the turn stays where it is.
void ChooseMap(const wchar_t* ini, char* code, size_t size, char* mode, size_t modeSize)
{
    code[0] = '\0';
    wchar_t state[MAX_PATH];
    swprintf(state, MAX_PATH, L"%smohw-bots-state.ini", ModuleDir());
    wchar_t onceWide[64] = {};
    GetPrivateProfileStringW(L"launch", L"Once", L"", onceWide, 64, state);
    if (onceWide[0])
    {
        WritePrivateProfileStringW(L"launch", L"Once", nullptr, state);
        char once[64] = {};
        WideCharToMultiByte(CP_ACP, 0, onceWide, -1, once, sizeof(once), nullptr, nullptr);
        modes::Entry entry;
        if (modes::ParseEntry(once, "TeamDeathMatch", "the restart's", &entry))
        {
            strncpy_s(code, size, entry.map.c_str(), _TRUNCATE);
            strncpy_s(mode, modeSize, entry.mode.c_str(), _TRUNCATE);
            Log("map: %s, %s again (ProCon's Restart Round; the rotation's turn stays where it was)", code,
                modes::DisplayName(entry.mode));
            return;
        }
    }
    std::vector<modes::Entry> rotation = modes::ReadRotation(ini);
    if (rotation.empty())
        return;
    const int count = static_cast<int>(rotation.size());
    int turn = static_cast<int>(GetPrivateProfileIntW(L"launch", L"NextMap", 0, state)) % count;
    strncpy_s(code, size, rotation[turn].map.c_str(), _TRUNCATE);
    strncpy_s(mode, modeSize, rotation[turn].mode.c_str(), _TRUNCATE);
    wchar_t following[16];
    swprintf(following, 16, L"%d", (turn + 1) % count);
    WritePrivateProfileStringW(L"launch", L"NextMap", following, state);
    const modes::Entry& after = rotation[(turn + 1) % count];
    Log("map: %s, %s (%d of %d in [launch] Map; next launch: %s, %s)", code, modes::DisplayName(rotation[turn].mode), turn + 1,
        count, after.map.c_str(), modes::DisplayName(after.mode));
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(module);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HMODULE image = GetModuleHandleW(nullptr);
    Log("==== mohw-bots loaded into %ls (image base %p) ====", exe, image);
    Log("version: %s", MOHW_BOTS_VERSION);
    Log("command line: %ls", GetCommandLineW());
    relaunch::WaitForPrevious();  // ProCon's Next Round started this one: the last game must let go of the RCON port first

    // The XInput proxy keeps working either way; the bot code only arms itself on the exact build it was written for.
    if (reinterpret_cast<uintptr_t>(image) != 0x400000)
    {
        Log("unexpected image base; bots disabled");
        return TRUE;
    }
    if (!eng::VerifySignatures())
    {
        Log("not the lanmohw.exe build these bots target (or its code is still encrypted); bots disabled");
        return TRUE;
    }

    wchar_t ini[MAX_PATH];
    wchar_t startupArgs[512] = {};
    swprintf(ini, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    // [launch] Map / Mode give the level and its game-mode layer; StartupArgs adds anything else (or, with no Map,
    // is the whole command line as before).
    char map[32] = {};
    char mode[64] = {};
    ChooseMap(ini, map, sizeof(map), mode, sizeof(mode));
    SetLaunchMap(map);
    if (!map[0])
    {
        wchar_t modeWide[64] = {};
        GetPrivateProfileStringW(L"launch", L"Mode", L"TeamDeathMatch", modeWide, 64, ini);
        WideCharToMultiByte(CP_ACP, 0, modeWide, -1, mode, sizeof(mode), nullptr, nullptr);
    }
    modes::SetCurrent(map, mode);
    bots::Init();  // after the mode is known: some of its rules are Team Deathmatch only
    GetPrivateProfileStringW(L"launch", L"StartupArgs", L"", startupArgs, 512, ini);
    char extra[512] = {};
    WideCharToMultiByte(CP_ACP, 0, startupArgs, -1, extra, sizeof(extra), nullptr, nullptr);
    char args[1024] = {};
    if (map[0])
        std::snprintf(args, sizeof(args), "-level MOHW/Levels/MP/%s/%s -Game.DefaultLayerInclusion GameMode=%s%s%s", map,
                      map, mode, extra[0] ? " " : "", extra);
    else
        std::snprintf(args, sizeof(args), "%s", extra);
    if (args[0])
        PatchStartupArgs(args);
    if (GetPrivateProfileIntW(L"launch", L"DisableOrigin", 0, ini) != 0)
        PatchDisableOrigin();
    if (GetPrivateProfileIntW(L"launch", L"ForceLocalServerBackend", 0, ini) != 0)
        PatchForceLocalServerBackend();
    if (int retries = static_cast<int>(GetPrivateProfileIntW(L"launch", L"ChallengeRetries", 0, ini)); retries != 0)
        PatchChallengeRetries(retries);
    if (GetPrivateProfileIntW(L"launch", L"LocalClientLink", 0, ini) != 0)
        PatchLocalClientLink();
    if (GetPrivateProfileIntW(L"bots", L"ForceServerAnimation", 1, ini) != 0)
        PatchForceServerAnimation();
    if (GetPrivateProfileIntW(L"bots", L"BotBulletsAsPlayers", 1, ini) != 0)
        PatchBotBulletsAsPlayers();
    if (GetPrivateProfileIntW(L"bots", L"BotHealthFromServer", 1, ini) != 0)
        PatchBotHealthFromServer();
    InstallOfflineStatsGuards();
    InstallMessageHook(GetPrivateProfileIntW(L"debug", L"TraceMessages", 0, ini) != 0);
    wchar_t layerInclusion[256] = {};
    char layerInclusionNarrow[256] = {};
    GetPrivateProfileStringW(L"launch", L"LayerInclusion", L"", layerInclusion, 256, ini);
    WideCharToMultiByte(CP_ACP, 0, layerInclusion, -1, layerInclusionNarrow, sizeof(layerInclusionNarrow), nullptr,
                        nullptr);
    if (!layerInclusionNarrow[0] && map[0])
        std::snprintf(layerInclusionNarrow, sizeof(layerInclusionNarrow), "GameMode=%s", mode);
    SetGameModeCounter(static_cast<int>(GetPrivateProfileIntW(L"launch", L"GameModeCounter", 0, ini)));
    SetHumanKicks(GetPrivateProfileIntW(L"launch", L"IdleKick", 0, ini) != 0,
                  GetPrivateProfileIntW(L"launch", L"TeamKillKick", 0, ini) != 0);
    InitOverrides(static_cast<int>(GetPrivateProfileIntW(L"launch", L"ConnectionTimeoutSeconds", 0, ini)),
                  layerInclusionNarrow, GetPrivateProfileIntW(L"spotting", L"ForceMinimapSpotting", 1, ini) != 0);

    if (!InstallServerUpdateHook())
        Log("hook install failed; bots disabled");
    return TRUE;
}
