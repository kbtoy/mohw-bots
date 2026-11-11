#include "limp.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "engine.h"
#include "log.h"

namespace limp {
namespace {

// ClientSoldierEntity's own vtable; its slot +0xD8 (0x98E950) is the soldier's health there, as the server soldier's
// (0xAD4070) is on the server: [soldier+0xD4] +0x18, else soldier +0x20. imagetest checks both.
constexpr uintptr_t kClientSoldierVtable = 0x27A09B8;
constexpr int kMaxIds = 64;
// The soldier's health module, and in it your game's copy of the server's health (vtable 0x2814A18 +0x7C, 0xDD55F0,
// written from each ghost update by 0xDCF1B0). imagetest checks both.
constexpr int kClientSoldier_HealthModule = 0x54C;
constexpr int kHealthModule_ServerHealth = 0x5AC;

struct Seen
{
    void* soldier = nullptr;  // your game's soldier for this player id
    DWORD deadSince = 0;      // when its health there first read 0 or below (0 = alive there)
    void* handled = nullptr;  // the body the server was last told to kill: never again for it
    bool aliveSeen = false;   // this soldier has had health there (a fresh one may read 0 before its first update)
    float copy = 0.0f;        // your game's copy of the server's health for it
    bool hasCopy = false;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Seen g_seen[kMaxIds];  // by player id (the same on both sides)
unsigned g_killMs = 500;
bool g_watchOnly = false;

// CheckCopy's, server thread only.
DWORD g_copyOffSince[kMaxIds] = {};
void* g_copyOffLogged[kMaxIds] = {};
int g_copyOffLogs = 0;
bool g_copyConfirmed = false;

}  // namespace

void Init()
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    g_killMs = GetPrivateProfileIntW(L"bots", L"LimpBodyKillMs", 500, path);
    g_watchOnly = GetPrivateProfileIntW(L"bots", L"BotHealthFromServer", 1, path) != 0;
    if (g_watchOnly)
        Log("limp bodies: watched only (BotHealthFromServer): nothing is killed, your game's estimate and its copy of "
            "the server's health are compared with the server");
    else if (g_killMs)
        Log("limp bodies: a bot your game shows dead for %u ms while the server has it alive is killed on the server too",
            g_killMs);
    else
        Log("limp bodies: not checked (LimpBodyKillMs=0)");
}

bool WatchOnly() { return g_watchOnly; }

void ClientTick()
{
    if (!g_killMs && !g_watchOnly)
        return;
    DWORD now = GetTickCount();
    Seen fresh[kMaxIds];
    for (int id = 0; id < kMaxIds; ++id)
    {
        void* player = nullptr;
        uint8_t ai = 0;
        void* soldier = nullptr;
        void* vtable = nullptr;
        float health = 0.0f;
        if (!eng::ClientPlayerById(id, &player) || !player || !eng::ReadByte(eng::At(player, eng::kPlayer_IsAI), &ai) ||
            !ai || !eng::ClientSoldierOf(player, &soldier) || !soldier || !eng::ReadPtr(soldier, &vtable) ||
            reinterpret_cast<uintptr_t>(vtable) != kClientSoldierVtable || !eng::GetSoldierHealth(soldier, &health))
            continue;
        fresh[id].soldier = soldier;
        fresh[id].aliveSeen = health > 0.0f;
        void* module = nullptr;
        fresh[id].hasCopy = eng::ReadPtr(eng::At(soldier, kClientSoldier_HealthModule), &module) && module &&
                            eng::ReadFloat(eng::At(module, kHealthModule_ServerHealth), &fresh[id].copy);
        if (health <= 0.0f)
            fresh[id].deadSince = now;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (int id = 0; id < kMaxIds; ++id)
    {
        fresh[id].handled = g_seen[id].handled;
        bool sameSoldier = fresh[id].soldier && fresh[id].soldier == g_seen[id].soldier;
        if (sameSoldier && g_seen[id].aliveSeen)
            fresh[id].aliveSeen = true;
        if (!fresh[id].aliveSeen)
            fresh[id].deadSince = 0;  // never alive there yet: not a body
        else if (fresh[id].soldier && fresh[id].soldier == fresh[id].handled)
            fresh[id].deadSince = 0;  // already dealt with
        // Still dead there on the same soldier: keep when it started.
        else if (fresh[id].deadSince && g_seen[id].deadSince && g_seen[id].soldier == fresh[id].soldier)
            fresh[id].deadSince = g_seen[id].deadSince;
        g_seen[id] = fresh[id];
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool ShownDeadOnlyThere(void* serverPlayer, void* serverSoldier, unsigned* outShownMs, float* outServerHealth)
{
    int id = -1;
    float health = 0.0f;
    if (!g_killMs || !eng::GetPlayerId(serverPlayer, &id) || id < 0 || id >= kMaxIds)
        return false;
    AcquireSRWLockShared(&g_lock);
    DWORD since = g_seen[id].deadSince;
    ReleaseSRWLockShared(&g_lock);
    if (!since)
        return false;
    DWORD shown = GetTickCount() - since;
    if (shown < g_killMs || !eng::GetSoldierHealth(serverSoldier, &health) || !(health > 0.0f))
        return false;
    *outShownMs = shown;
    *outServerHealth = health;
    AcquireSRWLockExclusive(&g_lock);
    g_seen[id].deadSince = 0;  // once per body
    g_seen[id].handled = g_seen[id].soldier;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

void CheckCopy(void* serverPlayer, void* serverSoldier, const char* name)
{
    int id = -1;
    float health = 0.0f;
    if (!g_watchOnly || !eng::GetPlayerId(serverPlayer, &id) || id < 0 || id >= kMaxIds ||
        !eng::GetSoldierHealth(serverSoldier, &health))
        return;
    AcquireSRWLockShared(&g_lock);
    bool hasCopy = g_seen[id].hasCopy;
    float copy = g_seen[id].copy;
    void* soldier = g_seen[id].soldier;
    ReleaseSRWLockShared(&g_lock);
    if (!hasCopy)
    {
        g_copyOffSince[id] = 0;
        return;
    }
    // Read up to 100 ms apart, so only a difference that lasts counts.
    if (std::fabs(copy - health) <= 5.0f)
    {
        g_copyOffSince[id] = 0;
        if (!g_copyConfirmed && copy < 95.0f && copy > 0.0f)
        {
            g_copyConfirmed = true;
            Log("bot %s: your game's copy of the server's health follows it (%.0f there, %.0f on the server): the "
                "first hit seen arrive",
                name, copy, health);
        }
        return;
    }
    DWORD now = GetTickCount();
    if (!g_copyOffSince[id])
        g_copyOffSince[id] = now;
    if (now - g_copyOffSince[id] >= 1000 && g_copyOffLogged[id] != soldier && g_copyOffLogs < 30)
    {
        g_copyOffLogged[id] = soldier;
        ++g_copyOffLogs;
        Log("bot %s: your game's copy of the server's health has been off for 1 s (%.0f there, %.0f on the server)",
            name, copy, health);
    }
}

}  // namespace limp
