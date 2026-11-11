#include "levelchange.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine.h"
#include "log.h"
#include "nav.h"
#include "overrides.h"
#include "rcon.h"
#include "rconsrv.h"
#include "relaunch.h"
#include "scoreboard.h"

namespace levelchange {
namespace {

// How long a change may take before the game is restarted instead. The first level load took ~13 s on the server.
constexpr DWORD kServerLimitMs = 150000;  // request -> the server has the new level
constexpr DWORD kClientLimitMs = 90000;   // server has it -> your game has it
constexpr DWORD kStalledMs = 180000;      // no server tick at all for this long (the server thread is stuck)
constexpr DWORD kLeaveLimitMs = 3000;     // request -> no bot player left (killing a soldier and deleting take a tick or two)

// 0 = no change; kLeaving = the bots are leaving; kLoading = the level has been asked for.
constexpr LONG kLeaving = 1;
constexpr LONG kLoading = 2;
volatile LONG g_pending = 0;
modes::Entry g_target;
std::string g_targetKey;
bool g_sameLevel = false;
int g_nextMapAfter = -1;
DWORD g_requestedAt = 0;
DWORD g_loadAskedAt = 0;
DWORD g_serverArrivedAt = 0;
bool g_inPlaceLevel = false;  // the level being played came in place (RestartAfterSpawnFaults)
void* g_oldServerLevel = nullptr;
bool g_serverLevelGone = false;  // the server had no level at some point since the request (a reload can reuse the address)
uint8_t g_flags[6] = {};
void* g_flagsLevel = nullptr;
bool g_flagsKnown = false;
volatile LONG g_lastServerTick = 0;
volatile LONG g_watchdog = 0;
volatile LONG g_fallingBack = 0;

// Your game's side (its own thread).
volatile LONG g_clientBaselineTaken = 0;
void* volatile g_clientBaseline = nullptr;
bool g_clientLevelGone = false;
volatile LONG g_clientArrived = 0;
volatile LONG g_clientReadable = 0;  // your game's level name has been read at least once (the offsets work)
bool g_clientLevelLogged = false;
constexpr DWORD kUnreadableClientMs = 20000;  // without that, the server's side is all there is to go by

std::string LevelPath(const std::string& map) { return "MOHW/Levels/MP/" + map + "/" + map; }

std::string Describe(const modes::Entry& e)
{
    const char* name = modes::MapName(e.map);
    return std::string(name ? name : e.map.c_str()) + ", " + modes::DisplayName(e.mode);
}

void* ServerLevel(void* context)
{
    void* level = nullptr;
    if (context)
        eng::ReadPtr(eng::At(context, eng::kCtx_Level), &level);
    return level;
}

// Your game's level and its name ("MOHW/Levels/MP/MP_03/MP_03"); the client's game context has the same layout.
bool ClientLevel(void** level, char* name, size_t size)
{
    void* context = nullptr;
    void* namePtr = nullptr;
    name[0] = '\0';
    if (!eng::ReadPtr(reinterpret_cast<const void*>(eng::kClientGameContextPtr), &context) || !context ||
        !eng::ReadPtr(eng::At(context, eng::kCtx_Level), level))
        return false;
    if (!*level || !eng::ReadPtr(eng::At(*level, eng::kLevel_Name), &namePtr) || !namePtr)
        return true;
    for (size_t i = 0; i + 1 < size; ++i)  // byte by byte: a short name may end just before an unreadable page
    {
        uint8_t c = 0;
        if (!eng::ProbeByte(static_cast<char*>(namePtr) + i, &c) || !c)
        {
            name[i] = '\0';
            break;
        }
        name[i] = static_cast<char>(c);
        name[i + 1] = '\0';
    }
    return true;
}

void WriteState(const wchar_t* key, const std::wstring& value)
{
    wchar_t state[MAX_PATH];
    swprintf(state, MAX_PATH, L"%smohw-bots-state.ini", ModuleDir());
    WritePrivateProfileStringW(L"launch", key, value.empty() ? nullptr : value.c_str(), state);
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void Done()
{
    InterlockedExchange(&g_pending, 0);
    g_inPlaceLevel = true;
    if (g_nextMapAfter >= 0)
    {
        WriteState(L"NextMap", std::to_wstring(g_nextMapAfter));
        rconsrv::SetNextMapIndex(g_nextMapAfter);
    }
    Log("level change: done, %.1f s after it was asked for", (GetTickCount() - g_requestedAt) / 1000.0);
    rcon::Announce("Now playing " + Describe(g_target) + ".");
}

void FallBack(const char* why)
{
    if (InterlockedExchange(&g_fallingBack, 1))
        return;
    Log("level change: %s; restarting the game with %s instead", why, Describe(g_target).c_str());
    rcon::Announce("Loading it in place didn't work; the game restarts with it instead.");
    RestartWith(g_target, g_nextMapAfter);
    InterlockedExchange(&g_pending, 0);  // only reached if the restart failed: let the bots play on wherever this is
    InterlockedExchange(&g_fallingBack, 0);
}

// The server thread is stuck in the change (no tick for kStalledMs): restart from here.
DWORD WINAPI Watchdog(void*)
{
    for (;;)
    {
        Sleep(1000);
        if (!g_pending)
            continue;
        const DWORD last = static_cast<DWORD>(g_lastServerTick);
        if (GetTickCount() - last > kStalledMs)
            FallBack("the server has done nothing for 3 minutes");
    }
}

// The second part, once the bots are gone: what the new level is read as (nav's key, the objectives, the mode rules)
// changes before it loads, then the load itself.
bool StartLoad(void* context)
{
    const modes::Entry was = {modes::CurrentMap(), modes::CurrentMode()};
    ChangeLayerInclusion(("GameMode=" + g_target.mode).c_str());
    SetLaunchMap(g_target.map.c_str());
    modes::SetCurrent(g_target.map, g_target.mode);
    g_oldServerLevel = ServerLevel(context);
    const std::string path = LevelPath(g_target.map);
    // The same map in another mode reloads in full too: the server's "already have it" check (0xAAB212) may only go by name.
    const bool reload = g_sameLevel || g_target.map == was.map;
    if (const char* failed = eng::RequestLevelLoad(path.c_str(), g_target.mode.c_str(), reload))
    {
        Log("level change: couldn't ask the server for %s (%s)", path.c_str(), failed);
        ChangeLayerInclusion(("GameMode=" + was.mode).c_str());
        SetLaunchMap(was.map.c_str());
        modes::SetCurrent(was.map, was.mode);
        return false;
    }
    g_loadAskedAt = GetTickCount();
    InterlockedExchange(&g_pending, kLoading);
    scoreboard::NewLevel();  // the rows start again with the level (nothing scores while it loads: the bots are gone)
    Log("level change: asked the server for %s %s (%s)%s", path.c_str(), g_target.mode.c_str(), Describe(g_target).c_str(),
        reload ? ", a full reload of this map" : "");
    return true;
}

}  // namespace

bool Pending() { return g_pending != 0; }

bool BotsLeaving() { return g_pending == kLeaving; }

bool LeavingLevel(void* level)
{
    return g_pending == kLoading && level && level == g_oldServerLevel && !g_serverLevelGone;
}

bool Request(const modes::Entry& target, bool sameLevel, int nextMapAfter)
{
    if (g_pending)
        return true;  // one at a time; the caller told ProCon already
    g_target = target;
    g_targetKey = target.map + "_" + target.mode;
    g_sameLevel = sameLevel;
    g_nextMapAfter = nextMapAfter;
    g_requestedAt = GetTickCount();
    g_loadAskedAt = 0;
    g_serverArrivedAt = 0;
    g_serverLevelGone = false;
    g_flagsKnown = false;
    InterlockedExchange(&g_lastServerTick, static_cast<LONG>(g_requestedAt));
    InterlockedExchange(&g_clientBaselineTaken, 0);
    InterlockedExchange(&g_clientArrived, 0);
    InterlockedExchange(&g_pending, kLeaving);
    Log("level change: %s asked for%s; the bots leave first (their players hold this level's soldier and kit data) and "
        "stand still until it's in",
        Describe(target).c_str(), sameLevel ? " (this level again)" : "");
    if (!InterlockedExchange(&g_watchdog, 1))
        if (HANDLE thread = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr))
            CloseHandle(thread);
    return true;
}

void ServerTick(void* server, void* context, const std::string& levelKey, size_t botPlayers)
{
    InterlockedExchange(&g_lastServerTick, static_cast<LONG>(GetTickCount()));
    if (!g_pending)
        return;
    const DWORD now = GetTickCount();
    if (g_pending == kLeaving)
    {
        if (botPlayers && now - g_requestedAt < kLeaveLimitMs)
            return;
        nav::OnLevel(std::string());  // saves the routes learned since the last save (also before a restart)
        if (botPlayers)
        {
            char why[96];
            std::snprintf(why, sizeof(why), "%u bot player(s) still in the game after 3 s",
                          static_cast<unsigned>(botPlayers));
            FallBack(why);
        }
        else
        {
            Log("level change: no bot players left (%.1f s)", (now - g_requestedAt) / 1000.0);
            if (!StartLoad(context))
                FallBack("the server couldn't be asked for the level");
        }
        return;
    }
    // The server's own load state, whenever it changes (for the first tries: which step it got to).
    uint8_t flags[6] = {};
    void* level = nullptr;
    if (server && eng::ReadServerLoadFlags(server, flags, &level) &&
        (!g_flagsKnown || std::memcmp(flags, g_flags, sizeof(flags)) != 0 || level != g_flagsLevel))
    {
        Log("level change: server load state +40 %u, +41 %u, +42 %u, +6C %u, +6D %u, +6E %u, level %p (%.1f s)", flags[0],
            flags[1], flags[2], flags[3], flags[4], flags[5], level, (now - g_loadAskedAt) / 1000.0);
        std::memcpy(g_flags, flags, sizeof(flags));
        g_flagsLevel = level;
        g_flagsKnown = true;
    }
    if (!g_serverArrivedAt)
    {
        void* current = ServerLevel(context);
        g_serverLevelGone |= current == nullptr;
        if (current && (current != g_oldServerLevel || g_serverLevelGone) && levelKey == g_targetKey)
        {
            g_serverArrivedAt = now;
            Log("level change: the server has %s after %.1f s; waiting for your game", levelKey.c_str(), (now - g_loadAskedAt) / 1000.0);
        }
        else if (now - g_loadAskedAt > kServerLimitMs)
        {
            nav::OnLevel(std::string());
            FallBack("the server didn't get the new level within 150 s");
        }
        return;
    }
    if (g_clientArrived || (!g_clientReadable && now - g_serverArrivedAt > kUnreadableClientMs))
        Done();
    else if (now - g_serverArrivedAt > kClientLimitMs)
    {
        nav::OnLevel(std::string());
        FallBack("your game didn't follow within 90 s");
    }
}

void ClientTick()
{
    if (!g_pending && g_clientLevelLogged)
        return;  // nothing to watch (this runs on every input poll)
    void* level = nullptr;
    char name[128];
    if (!ClientLevel(&level, name, sizeof(name)))
        return;
    if (name[0])
        InterlockedExchange(&g_clientReadable, 1);
    if (!g_clientLevelLogged && name[0])
    {
        g_clientLevelLogged = true;
        Log("level change: your game's level reads \"%s\" (%p)", name, level);
    }
    if (!g_pending || g_clientArrived)
        return;
    if (!g_clientBaselineTaken)
    {
        g_clientBaseline = level;
        g_clientLevelGone = level == nullptr;
        InterlockedExchange(&g_clientBaselineTaken, 1);
        return;
    }
    g_clientLevelGone |= level == nullptr;
    const std::string want = LevelPath(g_target.map);
    if (level && (level != g_clientBaseline || g_clientLevelGone) && _stricmp(name, want.c_str()) == 0)
    {
        Log("level change: your game has %s (%p)", name, level);
        InterlockedExchange(&g_clientArrived, 1);
    }
}

void RestartWith(const modes::Entry& target, int nextMapAfter)
{
    WriteState(L"Once", Wide(target.map + ":" + target.mode));
    if (nextMapAfter >= 0)
        WriteState(L"NextMap", std::to_wstring(nextMapAfter));
    if (!relaunch::Now())
    {
        WriteState(L"Once", L"");
        rcon::Announce("The game couldn't be restarted; close it and start it again");
    }
}

void RestartAfterSpawnFaults()
{
    if (!g_inPlaceLevel || g_pending || InterlockedExchange(&g_fallingBack, 1))
        return;
    const modes::Entry here = {modes::CurrentMap(), modes::CurrentMode()};
    Log("level change: bot spawns fault on this level, which was loaded in place; restarting the game with %s",
        Describe(here).c_str());
    rcon::Announce("Bots can't spawn on this level after loading it in place; the game restarts with it.");
    nav::OnLevel(std::string());
    RestartWith(here, -1);
    InterlockedExchange(&g_fallingBack, 0);
}

}  // namespace levelchange
