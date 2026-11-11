#include "spotting.h"

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <unordered_map>

#include "engine.h"
#include "hook.h"
#include "log.h"
#include "overrides.h"
#include "limp.h"
#include "melee.h"
#include "rcon.h"
#include "scoreboard.h"

namespace spotting {
namespace {

// ServerWeaponPlayerPrimaryFireShotSpawnedMessage (category 0x07038862, type 0xD3A8C934), sent by 0xBB2F80 for every
// shot: +0x28 the weapon's owner player (bots included), +0x2C the weapon.
constexpr uint32_t kCategoryWeapon = 0x07038862;
constexpr uint32_t kShotSpawned = 0xD3A8C934;

struct Settings
{
    bool revealOnFire = true;
    int fireSpotType = eng::kSpottedMQ9;  // minimap-only, as the MQ-9 UAV support action shows enemies
    bool friendlyBotsSpot = true;
    int sightSpotType = eng::kSpottedPassive;  // what a human teammate's game reports for an enemy in view
    bool clientSpots = false;              // also add the spot in your game directly (see ClientTick)
    int clientSpotType = eng::kSpottedMQ9;  // the minimap draws enemies with active or MQ9 entries
    bool clientDiagnostics = true;
};
Settings g_settings;
bool g_initialized = false;

CRITICAL_SECTION g_lock;
std::vector<void*> g_shooters;    // players that fired since the last tick
std::unordered_map<void*, DWORD> g_lastShot;  // player -> tick count of its last shot
std::vector<int> g_clientSpots;   // server player ids spotted since the last client tick

float g_clock = 0.0f;
std::unordered_map<void*, float> g_nextFireSpot;   // target player -> earliest time for the next spot
std::unordered_map<void*, float> g_nextSightSpot;
int g_logged = 0;

int ReadInt(const wchar_t* key, int fallback)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    return static_cast<int>(GetPrivateProfileIntW(L"spotting", key, fallback, path));
}

const brain::Actor* FindActor(const std::vector<brain::Actor>& actors, void* player)
{
    for (const brain::Actor& a : actors)
        if (a.player == player)
            return &a;
    return nullptr;
}

// Spots `target` for `spotter` at most once a second (the spot itself lasts the type's time and refreshes).
void Spot(const brain::Actor& target, void* spotter, int type, std::unordered_map<void*, float>& next,
          const char* why)
{
    auto it = next.find(target.player);
    if (it != next.end() && g_clock < it->second)
        return;
    next[target.player] = g_clock + 1.0f;
    void* component = nullptr;
    if (!eng::GetSpottingTarget(target.soldier, &component) || !component)
    {
        if (g_logged < 12)
        {
            ++g_logged;
            Log("spotting: soldier %p has no spotting target component", target.soldier);
        }
        return;
    }
    bool ok = eng::AddSpot(component, spotter, type);
    int id = -1;
    if (g_settings.clientSpots && eng::GetPlayerId(target.player, &id) && id >= 0)
    {
        EnterCriticalSection(&g_lock);
        if (g_clientSpots.size() < 128)
            g_clientSpots.push_back(id);
        LeaveCriticalSection(&g_lock);
    }
    if (g_logged < 12 || !ok)
    {
        ++g_logged;
        // The component data's spot times: +0x60 active, +0x64 passive, +0x88 MQ9.
        void* data = nullptr;
        float active = 0.0f, passive = 0.0f, mq9 = 0.0f;
        if (eng::ReadPtr(eng::At(component, 4), &data) && data)
        {
            eng::ReadFloat(eng::At(data, 0x60), &active);
            eng::ReadFloat(eng::At(data, 0x64), &passive);
            eng::ReadFloat(eng::At(data, 0x88), &mq9);
        }
        // And the server list sizes now ({begin, end} of 8-byte entries from +0x34, one list per SpottedType).
        uint32_t lists[5] = {};
        for (int k = 0; k < 5; ++k)
        {
            uint32_t begin = 0, end = 0;
            if (eng::ReadU32(eng::At(component, 0x34 + k * 0x10), &begin) &&
                eng::ReadU32(eng::At(component, 0x38 + k * 0x10), &end) && end >= begin)
                lists[k] = (end - begin) / 8;
        }
        Log("spotting: %s - soldier %p (player id %d) spotted (type %d) by player %p: %s (spot times: active %.1f s, "
            "passive %.1f s, MQ9 %.1f s; server lists %u/%u/%u/%u/%u)",
            why, target.soldier, id, type, spotter, ok ? "ok" : "FAILED", active, passive, mq9, lists[0], lists[1],
            lists[2], lists[3], lists[4]);
    }
}

}  // namespace

namespace {

// --- client side ------------------------------------------------------------------------------------------------
// The server spots above are replicated to your game (the component's network state 0x16917B0 -> 0x16B59C0) and the
// minimap draws an enemy whose client active / MQ9 list has entries - yet bots stayed invisible. As a fallback that
// doesn't depend on that replication, the same targets are also spotted in your game with its own addSpot
// (0x16B5D90, what your client does when you spot someone), on the main thread. The diagnostics show what your game's
// lists hold, to find out where the replicated spots get lost.

int g_clientLogged = 0;

// --- minimap instrumentation (client main thread) --------------------------------------------------------------
// MOHWIGHUDMinimapComp's per-player pass (0x1786230) calls the icon update (0x17850A0) for each enemy with flags
// (a5 / a6 / MQ9-spotted / active-spotted) and makes the icon visible when any is set. Counted here to see where an
// enemy bot drops out: pass not running, no enemy updates, no flags, or flags but the icon stays invisible.
using IconPassFn = void(__thiscall*)(void* self, float dt, void* localSoldier);
using UpdateIconFn = void(__thiscall*)(void* self, float dt, void* a2, void* soldier, int type, int f5, int f6,
                                       int f7, int f8);
IconPassFn g_iconPass = nullptr;
UpdateIconFn g_updateIcon = nullptr;
struct MinimapCounts
{
    int passes = 0, enemyUpdates = 0, f5 = 0, f6 = 0, mq9 = 0, active = 0, visible = 0, noIcon = 0, otherUpdates = 0;
};
MinimapCounts g_minimap;

void __fastcall IconPassDetour(void* self, void* /*edx*/, float dt, void* localSoldier)
{
    ++g_minimap.passes;
    g_iconPass(self, dt, localSoldier);
}

void __fastcall UpdateIconDetour(void* self, void* /*edx*/, float dt, void* a2, void* soldier, int type, int f5, int f6,
                                 int f7, int f8)
{
    g_updateIcon(self, dt, a2, soldier, type, f5, f6, f7, f8);
    if (type != 2)
    {
        ++g_minimap.otherUpdates;
        return;
    }
    ++g_minimap.enemyUpdates;
    g_minimap.f5 += (f5 & 0xFF) != 0;
    g_minimap.f6 += (f6 & 0xFF) != 0;
    g_minimap.mq9 += (f7 & 0xFF) != 0;
    g_minimap.active += (f8 & 0xFF) != 0;
    // The icon for this soldier: self+0x224 {begin, end} of icon pointers, icon+8 = soldier, +1 active, +0x44 alpha.
    void* begin = nullptr;
    void* end = nullptr;
    if (!eng::ProbePtr(eng::At(self, 0x224), &begin) || !eng::ProbePtr(eng::At(self, 0x228), &end) || !begin ||
        end < begin)
        return;
    for (void** p = static_cast<void**>(begin); p < static_cast<void**>(end); ++p)
    {
        void* icon = nullptr;
        void* owner = nullptr;
        if (!eng::ProbePtr(p, &icon) || !icon || !eng::ProbePtr(eng::At(icon, 8), &owner) || owner != soldier)
            continue;
        uint8_t active = 0;
        float alpha = 0.0f;
        eng::ProbeByte(eng::At(icon, 1), &active);
        eng::ReadFloat(eng::At(icon, 0x44), &alpha);
        if (active && alpha > 0.0f)
            ++g_minimap.visible;
        return;
    }
    ++g_minimap.noIcon;
}

// The icon pass's per-player gate: soldier+0xB0 (an interface) vfunc +0x1C must return true. -1 = fault.
int SoldierGate(void* soldier)
{
    using BoolFn = bool(__thiscall*)(void* self);
    void* iface = static_cast<char*>(soldier) + 0xB0;
    void** vtable = nullptr;
    void* fn = nullptr;
    if (!eng::ProbePtr(iface, reinterpret_cast<void**>(&vtable)) || !vtable || !eng::ProbePtr(vtable + 0x1C / 4, &fn) ||
        !eng::LooksLikeCode(reinterpret_cast<uintptr_t>(fn)))
        return -1;
    __try
    {
        return reinterpret_cast<BoolFn>(fn)(iface) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

void InstallMinimapHooks()
{
    void* pass = reinterpret_cast<void*>(eng::Address("Minimap_playerIconPass"));
    void* update = reinterpret_cast<void*>(eng::Address("Minimap_updateIcon"));
    // Both start push ebp; mov ebp, esp; and esp, -16 (6 bytes, position independent).
    if (pass)
        g_iconPass = reinterpret_cast<IconPassFn>(InlineHook(pass, 6, reinterpret_cast<const void*>(&IconPassDetour)));
    if (update)
        g_updateIcon =
            reinterpret_cast<UpdateIconFn>(InlineHook(update, 6, reinterpret_cast<const void*>(&UpdateIconDetour)));
    Log("spotting: minimap instrumentation %s", g_iconPass && g_updateIcon ? "hooked" : "FAILED");
}

// The minimap's per-player pass skips every enemy soldier whose player is AI (0x178635B, `je` after the isAI call ->
// past `cmp its team, your team; jne skip`): the campaign never shows enemy AI. Bots are AI players, so no spot, UAV
// or gunfire ever drew them. The je becomes jmp: enemy bots go through the same checks as enemy humans.
bool PatchMinimapEnemyAI()
{
    auto* site = reinterpret_cast<uint8_t*>(eng::Address("Minimap_skipEnemyAI"));
    if (!site)
        return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    site[0] = 0xEB;  // je rel8 -> jmp rel8
    VirtualProtect(site, 1, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, 1);
    return true;
}

const char* ClientSpotOne(int id, void* local)
{
    void* player = nullptr;
    void* soldier = nullptr;
    void* component = nullptr;
    if (!eng::ClientPlayerById(id, &player) || !player)
        return "no client player with that id";
    if (!eng::ClientSoldierOf(player, &soldier) || !soldier)
        return "no client soldier";
    if (!eng::ClientSpottingTarget(soldier, &component) || !component)
        return "no client spotting component";
    return eng::ClientAddSpot(component, local, g_settings.clientSpotType) ? nullptr : "client addSpot faulted";
}

// What your game knows about each player: team, whether its ClientPlayer is linked to a soldier (the minimap loops
// over players and skips those without one), and that soldier's spotting lists. Every 5 s for 2 minutes, then every
// 30 s.
void ClientDiagnostics(void* local)
{
    static DWORD last = 0;
    static DWORD first = 0;
    static int lines = 0;
    DWORD now = GetTickCount();
    if (!first)
        first = now;
    DWORD interval = now - first < 120000 ? 5000 : 30000;
    if (now - last < interval || lines >= 80)
        return;
    last = now;
    void* manager = eng::ClientPlayerManager();
    uint32_t localTeam = 0;
    void* begin = nullptr;
    void* end = nullptr;
    if (!manager || !eng::ReadU32(eng::At(local, eng::kPlayer_Team), &localTeam) ||
        !eng::ReadPtr(eng::At(manager, 0x98), &begin) || !eng::ReadPtr(eng::At(manager, 0x9C), &end) || !begin ||
        end < begin)
        return;
    size_t count = (static_cast<char*>(end) - static_cast<char*>(begin)) / sizeof(void*);
    int perTeam[3] = {};
    int enemies = 0, enemySoldiers = 0, enemySpotted = 0;
    char detail[900] = "";
    size_t used = 0;
    for (size_t i = 0; i < count && i < 64; ++i)
    {
        void* player = nullptr;
        uint32_t team = 0;
        if (!eng::ReadPtr(static_cast<void**>(begin) + i, &player) || !player ||
            !eng::ReadU32(eng::At(player, eng::kPlayer_Team), &team))
            continue;
        if (team < 3)
            ++perTeam[team];
        int id = -1;
        eng::GetPlayerId(player, &id);
        void* soldier = nullptr;
        eng::ClientSoldierOf(player, &soldier);
        bool enemy = team != localTeam && team != 0;
        if (enemy)
            ++enemies;
        if (!soldier)
        {
            if (used + 32 < sizeof(detail))
                used += std::snprintf(detail + used, sizeof(detail) - used, " [%d t%u%s no-soldier]", id, team,
                                      player == local ? " you" : "");
            continue;
        }
        if (enemy)
            ++enemySoldiers;
        void* component = nullptr;
        if (!eng::ClientSpottingTarget(soldier, &component) || !component)
        {
            if (used + 40 < sizeof(detail))
                used += std::snprintf(detail + used, sizeof(detail) - used, " [%d t%u no-spot-component]", id, team);
            continue;
        }
        uint8_t unspottable = 0;
        eng::ReadByte(eng::At(component, eng::kClientSpot_Unspottable), &unspottable);
        uint32_t sizes[6] = {};
        bool any = false;
        for (int k = 0; k < 6; ++k)
        {
            uint32_t b = 0, e = 0;
            if (eng::ReadU32(eng::At(component, eng::kClientSpot_Lists + k * 0x10), &b) &&
                eng::ReadU32(eng::At(component, eng::kClientSpot_Lists + 4 + k * 0x10), &e) && e >= b)
                sizes[k] = (e - b) / 8;
            any |= sizes[k] != 0;
        }
        if (any && enemy)
            ++enemySpotted;
        if (used + 64 < sizeof(detail))
            used += std::snprintf(detail + used, sizeof(detail) - used, " [%d t%u%s %u/%u/%u/%u/%u/%u%s%s]", id, team,
                                  player == local ? " you" : "", sizes[0], sizes[1], sizes[2], sizes[3], sizes[4],
                                  sizes[5], unspottable ? " unspottable" : "",
                                  enemy && SoldierGate(soldier) != 1 ? " gate-closed" : "");
    }
    ++lines;
    // The minimap's own gate (0x1786C70): your team, your soldier, and the HUD state (0x2A2E87C, -1 = none).
    int hudState = 0;
    eng::ReadU32(reinterpret_cast<const void*>(0x2A2E87C), reinterpret_cast<uint32_t*>(&hudState));
    void* localSoldier = nullptr;
    eng::ClientSoldierOf(local, &localSoldier);
    Log("spotting (your game): %u client players (team1 %d, team2 %d); you are team %u%s, HUD state %d; enemies %d, "
        "with a soldier %d, spotted %d; [player id, team, lists active/passive/victim/MQ9/flash/local]:%s",
        static_cast<unsigned>(count), perTeam[1], perTeam[2], localTeam, localSoldier ? "" : " (no soldier: minimap idle)",
        hudState, enemies, enemySoldiers, enemySpotted, detail);
    const MinimapCounts& m = g_minimap;
    Log("spotting (your game): minimap since last line: %d player passes, %d enemy icon updates (flags: a5 %d, a6 %d, "
        "MQ9 %d, active %d), %d with the icon visible, %d without an icon; %d friendly/other icon updates",
        m.passes, m.enemyUpdates, m.f5, m.f6, m.mq9, m.active, m.visible, m.noIcon, m.otherUpdates);
    g_minimap = MinimapCounts{};
}

}  // namespace

void OnDispatch(void* messageManager)
{
    if (!g_initialized)
        return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 100)
        return;
    // GameContext::dispatchMessage = [context]->dispatch: the client context's first field is its message manager.
    void* context = nullptr;
    void* clientManager = nullptr;
    if (!eng::ProbePtr(reinterpret_cast<const void*>(eng::kClientGameContextPtr), &context) || !context ||
        !eng::ProbePtr(context, &clientManager) || clientManager != messageManager)
        return;
    last = now;
    ClientTick();
}

void ClientTick()
{
    if (!g_initialized)
        return;
    static bool busy = false;  // a client call below may dispatch a message, which comes back through OnDispatch
    if (busy)
        return;
    struct Guard
    {
        Guard() { busy = true; }
        ~Guard() { busy = false; }
    } guard;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 100)
        return;
    last = now;
    std::vector<int> ids;
    EnterCriticalSection(&g_lock);
    ids.swap(g_clientSpots);
    LeaveCriticalSection(&g_lock);
    void* local = nullptr;
    if (!eng::ClientLocalPlayer(&local) || !local)
        return;
    static DWORD firstInGame = 0;
    static int ticks = 0;
    static bool rateLogged = false;
    if (!firstInGame)
        firstInGame = now;
    ++ticks;
    if (!rateLogged && now - firstInGame >= 10000)
    {
        Log("spotting (your game): client tick running on thread %lu, %d times in 10 s", GetCurrentThreadId(), ticks);
        rateLogged = true;
    }
    if (g_settings.clientSpots)
        for (int id : ids)
        {
            const char* problem = ClientSpotOne(id, local);
            if (g_clientLogged < 12 || (problem && g_clientLogged < 40))
            {
                ++g_clientLogged;
                Log("spotting (your game): player id %d -> %s", id, problem ? problem : "spotted (MQ9) in your game");
            }
        }
    if (g_settings.clientDiagnostics)
        ClientDiagnostics(local);
    scoreboard::ClientDiagnostics();
    scoreboard::ClientTick();
    limp::ClientTick();
    melee::ClientTick();
    rcon::ClientTick();
    ClientMapNameTick();
}

void Init()
{
    InitializeCriticalSection(&g_lock);
    g_settings.revealOnFire = ReadInt(L"RevealOnFire", 1) != 0;
    g_settings.fireSpotType = ReadInt(L"FireSpotType", g_settings.fireSpotType);
    g_settings.friendlyBotsSpot = ReadInt(L"FriendlyBotsSpot", 1) != 0;
    g_settings.sightSpotType = ReadInt(L"SightSpotType", g_settings.sightSpotType);
    g_settings.clientSpots = ReadInt(L"ClientSpots", 0) != 0;
    g_settings.clientSpotType = ReadInt(L"ClientSpotType", g_settings.clientSpotType);
    g_settings.clientDiagnostics = ReadInt(L"ClientDiagnostics", 1) != 0;
    if (g_settings.clientDiagnostics)
        InstallMinimapHooks();
    if (ReadInt(L"ShowEnemyBotsOnMinimap", 1) != 0)
        Log("spotting: minimap enemy-AI skip %s", PatchMinimapEnemyAI() ? "patched (spotted enemy bots are drawn)"
                                                                        : "FAILED (signature mismatch)");
    g_initialized = true;
    Log("spotting: enemy bots that fire %s (type %d), friendly bots spot what they see %s (type %d)",
        g_settings.revealOnFire ? "show on your minimap" : "not revealed", g_settings.fireSpotType,
        g_settings.friendlyBotsSpot ? "on" : "off", g_settings.sightSpotType);
}

void OnMessage(const void* message)
{
    const uint32_t* header = static_cast<const uint32_t*>(message);
    if (header[2] != kShotSpawned || header[1] != kCategoryWeapon)
        return;
    void* shooter = *reinterpret_cast<void* const*>(static_cast<const char*>(message) + 0x28);
    if (!shooter)
        return;
    EnterCriticalSection(&g_lock);
    bool known = false;
    for (void* p : g_shooters)
        known |= p == shooter;
    if (!known && g_shooters.size() < 64)
        g_shooters.push_back(shooter);
    g_lastShot[shooter] = GetTickCount();
    LeaveCriticalSection(&g_lock);
}

float SinceFired(void* player)
{
    if (!g_initialized)
        return 99.0f;
    float seconds = 99.0f;
    EnterCriticalSection(&g_lock);
    auto it = g_lastShot.find(player);
    if (it != g_lastShot.end())
    {
        DWORD age = GetTickCount() - it->second;
        if (age < 60000)
            seconds = age / 1000.0f;
    }
    LeaveCriticalSection(&g_lock);
    return seconds;
}

void Tick(void* human, int humanTeam, const std::vector<brain::Actor>& actors, const std::vector<Sighting>& sightings,
          float dt)
{
    g_clock += dt;
    std::vector<void*> shooters;
    EnterCriticalSection(&g_lock);
    shooters.swap(g_shooters);
    if (g_lastShot.size() > 64)  // forget shots over a minute old (dead bots' players are destroyed)
    {
        DWORD now = GetTickCount();
        for (auto it = g_lastShot.begin(); it != g_lastShot.end();)
            it = now - it->second > 60000 ? g_lastShot.erase(it) : std::next(it);
    }
    LeaveCriticalSection(&g_lock);

    if (g_settings.revealOnFire && human)
        for (void* p : shooters)
            if (const brain::Actor* a = FindActor(actors, p))
                if (a->isBot && a->team != humanTeam && a->team != 0)
                    Spot(*a, human, g_settings.fireSpotType, g_nextFireSpot, "fired");

    if (g_settings.friendlyBotsSpot)
        for (const Sighting& s : sightings)
            if (const brain::Actor* a = FindActor(actors, s.target))
                Spot(*a, s.spotter, g_settings.sightSpotType, g_nextSightSpot, "seen by a friendly bot");

    // Forget players that are gone (a dead bot's player is destroyed).
    if (g_nextFireSpot.size() + g_nextSightSpot.size() > 128)
    {
        std::unordered_map<void*, float>* tables[] = {&g_nextFireSpot, &g_nextSightSpot};
        for (auto* table : tables)
            for (auto it = table->begin(); it != table->end();)
                it = FindActor(actors, it->first) ? std::next(it) : table->erase(it);
    }
}

}  // namespace spotting
