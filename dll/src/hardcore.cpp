#include "hardcore.h"

#include <windows.h>

#include <cstdio>
#include <vector>

#include "engine.h"
#include "log.h"

namespace hardcore {
namespace {

struct Rule
{
    uintptr_t typeInfo;
    int offset;
    bool isFloat;
    float value;
    const char* name;
    bool released;
    float saved;     // what it was before Hardcore changed it
    bool hasSaved;
};

struct Lock
{
    CRITICAL_SECTION section;
    Lock() { InitializeCriticalSection(&section); }
};
Lock g_lock;  // ProCon can change the rules on the server thread while your game's thread applies them

class Locked
{
public:
    Locked() { EnterCriticalSection(&g_lock.section); }
    ~Locked() { LeaveCriticalSection(&g_lock.section); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
};

std::vector<Rule> g_rules;
volatile bool g_enabled = false;
volatile bool g_friendlyFire = false;
volatile bool g_leaderOnly = false;

int ReadInt(const wchar_t* key, int fallback)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    return static_cast<int>(GetPrivateProfileIntW(L"hardcore", key, fallback, path));
}

void AddFlag(uintptr_t typeInfo, int offset, const char* name)
{
    g_rules.push_back({typeInfo, offset, false, 1.0f, name, false, 0.0f, false});
}

// 0xB67E00 is the soldier's health multiplier: for a soldier that isn't AI it multiplies in the team's factor and
// ServerSettings.HumanHealthMultiplier (the setting behind BF3's vars.soldierHealth); an AI soldier skips both. Here an AI
// soldier skips only the team's factor. With the setting at 1, as it is outside Hardcore, nothing changes.
bool LetBotsHaveTheHealthSetting()
{
    auto* first = reinterpret_cast<uint8_t*>(eng::Address("ServerSoldier_healthHumanTest1"));
    auto* second = reinterpret_cast<uint8_t*>(eng::Address("ServerSoldier_healthHumanTest2"));
    if (!first || !second)
        return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(first, second + 4 - first, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    first[3] = static_cast<uint8_t>(second - (first + 4));  // je past both -> je to the second test
    second[2] = 0x90;                                       // the second test no longer skips
    second[3] = 0x90;
    VirtualProtect(first, second + 4 - first, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), first, second + 4 - first);
    return true;
}

void BuildRules()
{
    g_rules.clear();
    const uintptr_t game = eng::kTypeInfo_SyncedGameSettings;
    const uintptr_t bf = eng::kTypeInfo_SyncedBFSettings;
    const uintptr_t mohw = eng::kTypeInfo_SyncedMOHWSettings;
    if (ReadInt(L"FriendlyFire", 1) != 0)
        AddFlag(game, 0x1D, "EnableFriendlyFire");
    if (ReadInt(L"RegenerateHealth", 0) == 0)
        AddFlag(game, 0x1C, "DisableRegenerateHealth");
    if (ReadInt(L"KillCam", 0) == 0)
        AddFlag(mohw, 0x17, "NoKillCam");
    if (ReadInt(L"Spotting3D", 0) == 0)
        AddFlag(bf, 0x13, "No3dSpotting");
    if (ReadInt(L"NameTag", 0) == 0)
    {
        AddFlag(bf, 0x14, "NoNameTag");
        AddFlag(mohw, 0x12, "HudNoEnemyNametag");
        AddFlag(mohw, 0x18, "NoBuddyOutline");
    }
    if (ReadInt(L"Hud", 0) == 0)  // every HUD part but the minimap, which BF3's Hardcore keeps
    {
        AddFlag(mohw, 0x0D, "HudNoCrosshair");
        AddFlag(mohw, 0x0E, "HudNoExplosiveIcons");
        AddFlag(mohw, 0x0F, "HudNoHealthAmmo");
        AddFlag(mohw, 0x10, "HudNoPointsTracker");
        AddFlag(mohw, 0x11, "HudNoObituary");
        AddFlag(mohw, 0x13, "HudNoGameMode");
        AddFlag(mohw, 0x14, "HudNoBuddyInfo");
        AddFlag(mohw, 0x15, "HudNoUnlocks");
        AddFlag(mohw, 0x16, "HudNoClassAbility");
    }
    if (ReadInt(L"OnlySquadLeaderSpawn", 1) != 0)
        AddFlag(bf, 0x15, "OnlySquadLeaderSpawn");
    int health = ReadInt(L"SoldierHealth", 60);
    if (health > 0)
        g_rules.push_back({eng::kTypeInfo_ServerSettings, 0x2C, true, static_cast<float>(health) / 100.0f, "HumanHealthMultiplier", false, 0.0f, false});
    Log("hardcore: on, %u settings (soldier health %d%%)", static_cast<unsigned>(g_rules.size()), health);
}

void PutBack()
{
    for (const Rule& rule : g_rules)
    {
        void* container = nullptr;
        if (rule.released || !rule.hasSaved || !eng::GetSettingsContainer(rule.typeInfo, &container) || !container)
            continue;
        char* field = static_cast<char*>(container) + rule.offset;
        if (rule.isFloat)
        {
            eng::WriteBytes(field, &rule.saved, sizeof(rule.saved));
            Log("hardcore: %s back to %.2f", rule.name, rule.saved);
        }
        else
        {
            uint8_t value = rule.saved != 0.0f ? 1 : 0;
            eng::WriteBytes(field, &value, 1);
            Log("hardcore: %s back to %u", rule.name, value);
        }
    }
}

}  // namespace

void Init()
{
    if (!LetBotsHaveTheHealthSetting())
        Log("hardcore: bots keep full health whatever the soldier health setting says (a signature didn't match)");

    Locked lock;
    g_enabled = ReadInt(L"Enabled", 0) != 0;
    if (g_enabled)
        BuildRules();
    else
        Log("hardcore: off ([hardcore] Enabled=0)");
}

bool Enabled() { return g_enabled; }

bool Set(bool on)
{
    {
        Locked lock;
        if (on == g_enabled)
            return false;
        if (on)
        {
            BuildRules();
        }
        else
        {
            PutBack();
            g_rules.clear();
            Log("hardcore: off");
        }
        g_enabled = on;
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    if (!WritePrivateProfileStringW(L"hardcore", L"Enabled", on ? L"1" : L"0", path))
        Log("hardcore: couldn't save this in mohw-bots.ini (%lu); the next launch goes by what's there", GetLastError());
    Apply();
    return true;
}

bool Apply()
{
    Locked lock;
    bool all = true;
    for (Rule& rule : g_rules)
    {
        if (rule.released)
            continue;
        void* container = nullptr;
        if (!eng::GetSettingsContainer(rule.typeInfo, &container) || !container)
        {
            all = false;
            continue;
        }
        char* field = static_cast<char*>(container) + rule.offset;
        if (rule.isFloat)
        {
            float current = 0.0f;
            bool read = eng::ReadFloat(field, &current);
            if (read && !rule.hasSaved)
            {
                rule.saved = current;
                rule.hasSaved = true;
            }
            if (read && current != rule.value)
            {
                eng::WriteBytes(field, &rule.value, sizeof(rule.value));
                Log("hardcore: %s %.2f -> %.2f", rule.name, current, rule.value);
            }
        }
        else
        {
            uint8_t current = 0;
            bool read = eng::ReadByte(field, &current);
            if (read && !rule.hasSaved)
            {
                rule.saved = current ? 1.0f : 0.0f;
                rule.hasSaved = true;
            }
            if (read && current != 1)
            {
                uint8_t on = 1;
                eng::WriteBytes(field, &on, 1);
                Log("hardcore: %s %u -> 1", rule.name, current);
            }
        }
    }
    void* game = nullptr;
    void* bf = nullptr;
    uint8_t flag = 0;
    if (eng::GetSettingsContainer(eng::kTypeInfo_SyncedGameSettings, &game) && game && eng::ReadByte(eng::At(game, 0x1D), &flag))
        g_friendlyFire = flag != 0;
    else
        all = false;
    if (eng::GetSettingsContainer(eng::kTypeInfo_SyncedBFSettings, &bf) && bf && eng::ReadByte(eng::At(bf, 0x15), &flag))
        g_leaderOnly = flag != 0;
    else
        all = false;
    return all;
}

bool Owns(uintptr_t typeInfo, int offset)
{
    Locked lock;
    for (const Rule& rule : g_rules)
        if (rule.typeInfo == typeInfo && rule.offset == offset && !rule.released)
            return true;
    return false;
}

void Release(uintptr_t typeInfo, int offset)
{
    Locked lock;
    for (Rule& rule : g_rules)
        if (rule.typeInfo == typeInfo && rule.offset == offset && !rule.released)
        {
            rule.released = true;
            Log("hardcore: %s was set from ProCon; left alone from now on", rule.name);
        }
}

bool FriendlyFire() { return g_friendlyFire; }
bool LeaderSpawnOnly() { return g_leaderOnly; }

}  // namespace hardcore
