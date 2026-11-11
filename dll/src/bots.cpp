// First-test bot manager: create AI players with the engine's own pipeline (re/RE_NOTES.md, "Putting it together"),
// spawn them, and drive their EntryInput. Everything runs on the server thread inside Server::update.
#include "bots.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "brain.h"
#include "nav.h"
#include "engine.h"
#include "hook.h"
#include "log.h"
#include "overrides.h"
#include "combatlog.h"
#include "grenades.h"
#include "limp.h"
#include "hardcore.h"
#include "levelchange.h"
#include "melee.h"
#include "modes.h"
#include "objectives.h"
#include "rcon.h"
#include "scoreboard.h"
#include "scoring.h"
#include "spotting.h"

namespace bots {
namespace {

using eng::At;

struct Config
{
    int keySpawnHere = VK_F8;   // add a bot; the engine's spawn-point chooser places it
    int keySpawnAtMe = VK_F7;   // add a bot 3 m in front of your soldier
    int keyToggleMove = VK_F9;  // pause / resume the bot brain (paused bots stand still and hold fire)
    int keyDump = VK_F11;       // write players and bots to the log
    int team = 0;               // 0 = opposite of the host's team
    int humanTeam = 1;          // team for human players still on team 0 (offline nobody assigns one); 0 = leave
    int kitIndex = -1;          // -1 = random kit per bot
    int keySpawnFriendly = VK_F6;  // add a bot on your team at a spawn point the engine picks
    int blueprint = 0;          // 0 = Soldier if set, else CustomizedSoldier; 1 = Soldier; 2 = CustomizedSoldier
    int unlocks = 2;            // default unlocks for bots: 0 none, 1 kit + appearance, 2 kit + appearance + weapons
    int autoRespawnSeconds = 5;  // 0 = never respawn
    int alwaysStep = 1;          // single-step bot soldiers every tick (fun-bots does; without it bots never walk)
    int fireteams = 1;           // pair players into fireteams (offline nobody does; everyone stayed squad 5)
    int spawnOnBuddy = 1;        // spawn next to the fireteam buddy when it is out of combat (else fall back)
    int buddySafeMeters = 25;    // an enemy this close to the buddy counts as combat
    int fallBackToOwnBase = 1;   // a fall-back spawn uses the bot's own team's spawn points only
    int sharedSpawnWhenCamped = 1;  // ...unless that base is camped: then the map's shared points
    int campedEnemies = 2;          // camped: this many enemies near the base for CampedSeconds (0 = don't look)
    int campedSeconds = 10;
    int sharedSpawnSafeMeters = 25;  // a shared point this close to an enemy isn't used
    int campedSpawnDeaths = 3;      // or this many players killed there soon after spawning, in 30 s (0 = don't count)
    int demoMiniGrenades = 1;    // Demolitions bots carry V40 mini grenades instead of remote charges
    int keepPlayers = 1;         // a dead bot keeps its player (the game deletes dead AI players) and respawns as it
    int teamSize = 10;           // players per team the fill tops up to with bots (you count); 0 = add bots by key only
};

struct Bot
{
    void* player = nullptr;
    void* input = nullptr;  // owned by us; the engine never frees a player's input
    int team = 0;
    float deadSeconds = 0.0f;
    bool hadSoldier = false;
    bool loggedStep = false;
    bool stepDisabled = false;
    int kitType = 0;  // MPKitType of its kit (4 = sniper, 6 = support): fire rhythm
    LONG diedInRound = -1;  // one life per round (Home Run): the round it died in waits out
    bool toldOut = false;
    bool killedToLeave = false;  // its soldier was killed for an in-place level change; its player goes with the death
    bool deleteFailed = false;   // deleting its player for the change faulted: left alone (the change restarts the game)
    bool waitingForLevel = false;  // due to respawn while the server has no level (logged once)
    brain::State brain;
    char name[32] = {};
};


Config g_config;
std::vector<Bot> g_bots;
// Every bot player ever created -> name / kit type: a dead bot's player is destroyed and it returns as a new one, so
// the combat log can still name the old player's events.
std::unordered_map<void*, std::pair<std::string, int>> g_botHistory;

// A dead bot waiting to come back as a new player with the same name and team. MOHW destroys an AI player when it dies
// (ServerPlayerKilled -> ReleasingLevel -> ServerPlayerDestroy), as for campaign enemies - unless KeepPlayers stops it
// (then a bot respawns as itself and this is only the fall-back for a player that went anyway).
struct PendingRespawn
{
    int team = 0;
    float waited = 0.0f;
    LONG round = -1;  // the round it died in (one life per round)
    char name[32] = {};
};
std::vector<PendingRespawn> g_respawns;
bool g_keyDown[256] = {};
void* g_lastContext = nullptr;
// The round is over (ServerRoundOverMessage): the results screens are up and you can't move, so bots stand still and
// don't respawn until a new round (ServerRoundResetMessage) or level. They used to play on behind the results.
constexpr uint32_t kCategoryRound = 0xD93DD7C2;
constexpr uint32_t kRoundOver = 0x16BABB47;
constexpr uint32_t kRoundReset = 0x4BB8DE5C;
constexpr uint32_t kSubRoundOver = 0x42983801;  // rounds within a match (Home Run); TDM never sends it
constexpr uint32_t kHalfTime = 0x73F6161F;
volatile LONG g_roundOver = 0;
// Bots stand still: the round is over, or the level is changing in place (levelchange.h).
bool StandDown() { return g_roundOver || levelchange::Pending(); }
// Bot spawns that faulted inside the game on this level. Each leaves a half-made soldier behind: after the first in-place
// level change, 76 of them (every bot's respawn every 5 s, from players holding the old level's assets) froze the game. So
// after kSpawnFaultLimit no more bots spawn on this level.
constexpr int kSpawnFaultLimit = 2;
int g_spawnFaults = 0;
bool g_spawnsHalted = false;
std::string g_levelKey;  // the server level's key as nav has it (UpdateNavLevel)
// Round boundaries (a reset, a sub-round over, half time) and when the last came: in Home Run a bot that died waits for
// the next one, as players do. A soldier lost within kBoundaryGraceMs of one went with the round change, not a death.
volatile LONG g_roundBoundaries = 0;
volatile LONG g_lastBoundaryTick = 0;
constexpr DWORD kBoundaryGraceMs = 10000;

// The round a soldier lost now counts as lost in.
LONG RoundOfDeath()
{
    LONG round = g_roundBoundaries;
    if (round > 0 && GetTickCount() - static_cast<DWORD>(g_lastBoundaryTick) < kBoundaryGraceMs)
        --round;  // the round change took it: it may play the new round
    return round;
}

// One life per round: still the round it died in.
bool SitsOutRound(LONG diedInRound) { return modes::OneLifePerRound() && diedInRound == g_roundBoundaries; }
std::vector<void*> g_kicked;  // bots leaving at their next death ([rcon], admin.kickPlayer)
int g_nextBotNumber = 1;
// [bots] Names: a new bot takes a random one no other bot (alive or waiting to respawn) has; "Bot07"-style numbers
// once they're all taken.
std::vector<std::string> g_names;
constexpr wchar_t kDefaultNames[] = L"Dingus,Ol Red,Slick,Rufus,Drumstick,Crackers,Doug,Witou Low,Wang Chung,Ventura,"
                                    L"Meathead,Ooga Booga,Sumting Wong,Billy,Whatta Chatta,Barttle Doo,Left Eye,"
                                    L"Cornwallas,Jones";

int ReadIni(const wchar_t* key, int fallback)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    return static_cast<int>(GetPrivateProfileIntW(L"bots", key, fallback, path));
}

uint32_t NextRandom()
{
    static uint32_t state = GetTickCount() | 1u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// Audible result so testers don't need the log to know whether a key did something.
void Feedback(bool ok)
{
    MessageBeep(ok ? MB_OK : MB_ICONHAND);
}

// Feedback for a key press; silent for automatic respawns (and kSilent: bots the team fill adds).
void KeyFeedback(const PendingRespawn* respawn, bool ok)
{
    if (!respawn)
        Feedback(ok);
}
const PendingRespawn kSilent;

// Edge-triggered, and only while the game window has focus.
bool KeyPressed(int vk)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = down && !g_keyDown[vk & 0xFF];
    g_keyDown[vk & 0xFF] = down;
    return pressed;
}

std::vector<void*> Players(void* manager)
{
    std::vector<void*> players;
    void* begin = nullptr;
    void* end = nullptr;
    if (!eng::ReadPtr(At(manager, eng::kMgr_PlayersBegin), &begin) || !eng::ReadPtr(At(manager, eng::kMgr_PlayersEnd), &end))
        return players;
    size_t count = (static_cast<char*>(end) - static_cast<char*>(begin)) / sizeof(void*);
    if (!begin || count > 256)
        return players;
    for (size_t i = 0; i < count; ++i)
    {
        void* p = nullptr;
        if (eng::ReadPtr(static_cast<void**>(begin) + i, &p) && p)
            players.push_back(p);
    }
    return players;
}

bool IsAI(void* player)
{
    uint8_t ai = 0;
    return eng::ReadByte(At(player, eng::kPlayer_IsAI), &ai) && ai != 0;
}

void* SoldierOf(void* player)
{
    void* soldier = nullptr;
    eng::ReadPtr(At(player, eng::kPlayer_Soldier), &soldier);
    return soldier;
}

Bot* FindBot(void* player)
{
    for (Bot& bot : g_bots)
        if (bot.player == player)
            return &bot;
    return nullptr;
}

void ReadNames()
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    wchar_t text[2048] = {};
    GetPrivateProfileStringW(L"bots", L"Names", kDefaultNames, text, 2048, path);
    g_names.clear();
    std::string name;
    for (const wchar_t* c = text;; ++c)
    {
        if (*c == L',' || *c == L'\0')
        {
            while (!name.empty() && name.back() == ' ')
                name.pop_back();
            if (!name.empty() && name.size() < sizeof(Bot::name))
                g_names.push_back(name);
            name.clear();
            if (*c == L'\0')
                break;
        }
        else if (!(name.empty() && *c == L' ') && *c < 0x80)
            name += static_cast<char>(*c);
    }
}

std::string PickName()
{
    std::vector<const std::string*> free;
    for (const std::string& n : g_names)
    {
        bool taken = false;
        for (const Bot& b : g_bots)
            taken |= n == b.name;
        for (const PendingRespawn& r : g_respawns)
            taken |= n == r.name;
        if (!taken)
            free.push_back(&n);
    }
    if (!free.empty())
        return *free[NextRandom() % free.size()];
    char number[16];
    std::snprintf(number, sizeof(number), "Bot%02d", g_nextBotNumber++);
    return number;
}

// A bot's name, "you" (a human player) or the pointer, for the log.
const char* NameOf(void* player, char* buffer, size_t size)
{
    if (const Bot* bot = FindBot(player))
        return bot->name;
    uint8_t ai = 1;
    if (player && eng::ProbeByte(At(player, eng::kPlayer_IsAI), &ai) && !ai)
        return "you";
    std::snprintf(buffer, size, "%p", player);
    return buffer;
}

void* FindHost(const std::vector<void*>& players)
{
    for (void* p : players)
        if (!IsAI(p))
            return p;
    return nullptr;
}

void* PlayerManager(void* context)
{
    void* manager = nullptr;
    eng::ReadPtr(At(context, eng::kCtx_PlayerManager), &manager);
    return manager;
}

// TeamData for a team id, or null. Teams live at level+0x64 as TeamEntityData*[count].
void* TeamDataFor(void* context, int team, uint32_t* outTeamCount)
{
    void* level = nullptr;
    void* teams = nullptr;
    uint32_t count = 0;
    if (!eng::ReadPtr(At(context, eng::kCtx_Level), &level) || !level || !eng::ReadPtr(At(level, eng::kLevel_Teams), &teams) ||
        !teams || !eng::ReadU32(At(teams, eng::kTeams_Count), &count))
        return nullptr;
    if (outTeamCount)
        *outTeamCount = count;
    void* teamEntityData = nullptr;
    void* teamData = nullptr;
    if (team < 0 || static_cast<uint32_t>(team) >= count || !eng::ReadPtr(static_cast<void**>(teams) + team, &teamEntityData) ||
        !teamEntityData || !eng::ReadPtr(At(teamEntityData, eng::kTeamEntityData_Team), &teamData))
        return nullptr;
    return teamData;
}

void* ChooseBlueprint(void* teamData, int* outKitCount)
{
    void* soldier = nullptr;
    void* customized = nullptr;
    void* kits = nullptr;
    eng::ReadPtr(At(teamData, eng::kTeamData_Soldier), &soldier);
    eng::ReadPtr(At(teamData, eng::kTeamData_CustomizedSoldier), &customized);
    *outKitCount = -1;
    uint32_t kitCount = 0;
    if (eng::ReadPtr(At(teamData, eng::kTeamData_SoldierCustomization), &kits) && kits &&
        eng::ReadU32(static_cast<char*>(kits) - 4, &kitCount))
        *outKitCount = static_cast<int>(kitCount);
    Log("  TeamData %p: Soldier=%p CustomizedSoldier=%p kits=%d", teamData, soldier, customized, *outKitCount);
    // The game spawns human players with Soldier. CustomizedSoldier looks like the deploy screen's preview body
    // (bots spawned with it were invisible and took no damage), so it's opt-in only.
    switch (g_config.blueprint)
    {
    case 1: return soldier;
    case 2: return customized;
    default: return soldier ? soldier : customized;
    }
}

// fb::Array<T*>: element pointer, count in the dword before the first element.
uint32_t ArrayCount(void* elements)
{
    uint32_t count = 0;
    if (!elements || !eng::ReadU32(static_cast<char*>(elements) - 4, &count) || count > 256)
        return 0;
    return count;
}

void* ArrayAt(void* elements, uint32_t index)
{
    void* item = nullptr;
    eng::ReadPtr(static_cast<void**>(elements) + index, &item);
    return item;
}

// "kit_sniper_unlock"-style short name of an asset (after the last '/'), for the log.
void ShortAssetName(void* asset, char* out, size_t size)
{
    void* name = nullptr;
    char text[128] = "?";
    if (asset && eng::ReadPtr(At(asset, eng::kAsset_Name), &name) && name)
    {
        size_t k = 0;
        for (uint8_t c = 0; k + 1 < sizeof(text) && eng::ProbeByte(static_cast<char*>(name) + k, &c) && c; ++k)
            text[k] = static_cast<char>(c);
        text[k] = '\0';
    }
    const char* slash = std::strrchr(text, '/');
    std::snprintf(out, size, "%s", slash ? slash + 1 : text);
}

// TeamData.SoldierCustomization[kitIndex] (a SoldierCustomizationAsset), or null.
void* KitFor(void* teamData, int kitIndex)
{
    void* kits = nullptr;
    if (!eng::ReadPtr(At(teamData, eng::kTeamData_SoldierCustomization), &kits) || !kits ||
        static_cast<uint32_t>(kitIndex) >= ArrayCount(kits))
        return nullptr;
    return ArrayAt(kits, kitIndex);
}

// A kit's WeaponTable parts (CustomizationUnlockParts*[]; a part's index is its weapon slot), or null.
void** WeaponParts(void* kit, uint32_t* count)
{
    void* table = nullptr;
    void* parts = nullptr;
    *count = 0;
    if (!kit || !eng::ReadPtr(At(kit, eng::kKit_WeaponTable), &table) || !table ||
        !eng::ReadPtr(At(table, eng::kCustomizationTable_UnlockParts), &parts) || !parts)
        return nullptr;
    *count = ArrayCount(parts);
    return static_cast<void**>(parts);
}

// Short name of a part's first selectable unlock - the one the game's default loadout takes ("" if none).
void FirstUnlockName(void* part, char* out, size_t size)
{
    void* selectable = nullptr;
    out[0] = '\0';
    if (part && eng::ReadPtr(At(part, eng::kUnlockParts_Selectable), &selectable) && ArrayCount(selectable))
        ShortAssetName(ArrayAt(selectable, 0), out, size);
}

bool WritePtr(void** address, void* value)
{
    __try
    {
        *address = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

constexpr uint32_t kKitDemolitions = 3;  // MPKitType
constexpr uint32_t kKitSupport = 6;

using PartSwaps = std::vector<std::pair<void**, void*>>;

// Demolitions bots get the Support kit's V40 mini grenades instead of remote charges: their charges only go off from
// the detonator, a weapon bots never switch to. Both sit in slot 6 of their kit's WeaponTable (the grenade slot); the
// detonator is slot 3, empty in the Support kit. The game's default loadout (0xB41B70) takes the first weapon of each
// part, so while it runs for this bot the Demolitions kit's two parts point at the Support kit's. Returns what to put
// back afterwards.
PartSwaps BorrowSupportGrenades(const Bot& bot, void* teamData, void* kit)
{
    PartSwaps swapped;
    if (!g_config.demoMiniGrenades || static_cast<uint32_t>(bot.kitType) != kKitDemolitions)
        return swapped;
    void* kits = nullptr;
    void* support = nullptr;
    if (eng::ReadPtr(At(teamData, eng::kTeamData_SoldierCustomization), &kits) && kits)
        for (uint32_t i = 0, n = ArrayCount(kits); i < n && !support; ++i)
        {
            void* k = ArrayAt(kits, i);
            uint32_t type = 0;
            if (k && eng::ReadU32(At(k, eng::kKit_MPKitType), &type) && type == kKitSupport)
                support = k;
        }
    uint32_t demoCount = 0, supportCount = 0;
    void** demo = WeaponParts(kit, &demoCount);
    void** from = WeaponParts(support, &supportCount);
    if (!demo || !from)
    {
        Log("  [%s] Demolitions: no Support kit on its team to borrow grenades from; keeps the remote charges", bot.name);
        return swapped;
    }
    char grenade[64] = "";
    bool noDetonator = false;
    for (uint32_t slot = 0; slot < demoCount && slot < supportCount; ++slot)
    {
        char ours[64], theirs[64];
        FirstUnlockName(demo[slot], ours, sizeof(ours));
        FirstUnlockName(from[slot], theirs, sizeof(theirs));
        bool charge = _stricmp(ours, "CA_THROWABLE_EXPLOSIVES") == 0 && _strnicmp(theirs, "W_GR_", 5) == 0;
        bool detonator = _stricmp(ours, "CA_THROWABLE_EXPLOSIVES_DETONATOR") == 0 && !theirs[0];
        if (!(charge || detonator))
            continue;
        void* original = demo[slot];
        if (!WritePtr(&demo[slot], from[slot]))
            continue;
        swapped.push_back({&demo[slot], original});
        if (charge)
            std::snprintf(grenade, sizeof(grenade), "%s (slot %u)", theirs, slot);
        noDetonator |= detonator;
    }
    Log("  [%s] Demolitions: %s%s", bot.name, grenade[0] ? grenade : "no grenade swapped in (kit layout not as expected)",
        grenade[0] ? (noDetonator ? " instead of remote charges, no detonator" : " instead of remote charges") : "");
    return swapped;
}

void RestoreParts(const PartSwaps& swapped)
{
    for (const auto& s : swapped)
        WritePtr(s.first, s.second);
}

// ServerPlayer+0x524 (selected UnlockAssets): count, and the names in one log line.
size_t UnlockCount(void* player)
{
    void* begin = nullptr;
    void* end = nullptr;
    if (!eng::ReadPtr(At(player, eng::kPlayer_Unlocks), &begin) || !eng::ReadPtr(At(player, eng::kPlayer_Unlocks + 4), &end) ||
        end < begin)
        return 0;
    return (static_cast<char*>(end) - static_cast<char*>(begin)) / sizeof(void*);
}

void LogPlayerUnlocks(void* player, const char* label)
{
    void* begin = nullptr;
    size_t count = UnlockCount(player);
    eng::ReadPtr(At(player, eng::kPlayer_Unlocks), &begin);
    char names[1024] = "";
    size_t used = 0;
    for (size_t i = 0; begin && i < count && i < 48 && used + 64 < sizeof(names); ++i)
    {
        char shortName[64];
        ShortAssetName(ArrayAt(begin, static_cast<uint32_t>(i)), shortName, sizeof(shortName));
        used += std::snprintf(names + used, sizeof(names) - used, "%s%s", i ? ", " : "", shortName);
    }
    Log("%s (%u): %s", label, static_cast<unsigned>(count), names);
}

// Every automatic unlock plus the first selectable unlock of every part of the kit's visual, specialization and
// weapon tables: the kit unlock, a soldier appearance, and a default loadout - what a player has without touching the
// customisation screens. Spawn customisation builds the soldier from ServerPlayer+0x524; a bot with an empty list
// spawns with no body (invisible, can't be hit), and weapons without the kit/appearance faulted the spawn.
// Returns how many unlocks were added.
int GiveDefaultUnlocks(Bot& bot, void* teamData, int kitIndex)
{
    void* kit = KitFor(teamData, kitIndex);
    if (!kit)
        return 0;
    int added = 0;
    char names[512] = "";
    size_t used = 0;
    int skipped = 0;
    char skippedNames[256] = "";
    size_t skippedUsed = 0;
    auto add = [&](void* unlock) {
        if (!unlock)
            return;
        // Only UnlockAssets belong in this list (spawn reads their LinkedTo); weapon unlocks go to the weapon set.
        if (!eng::IsKindOf(unlock, eng::kTypeInfo_UnlockAsset))
        {
            ++skipped;
            char shortName[64];
            ShortAssetName(unlock, shortName, sizeof(shortName));
            if (skippedUsed + 64 < sizeof(skippedNames))
                skippedUsed += std::snprintf(skippedNames + skippedUsed, sizeof(skippedNames) - skippedUsed, "%s%s",
                                             skipped > 1 ? ", " : "", shortName);
            return;
        }
        if (!eng::AddPlayerUnlock(bot.player, unlock))
            return;
        ++added;
        char shortName[64];
        ShortAssetName(unlock, shortName, sizeof(shortName));
        if (used + 64 < sizeof(names))
            used += std::snprintf(names + used, sizeof(names) - used, "%s%s", added > 1 ? ", " : "", shortName);
    };
    const int tables[] = {eng::kKit_VisualTable, eng::kKit_SpecializationTable, eng::kKit_WeaponTable};
    for (int tableOffset : tables)
    {
        if (g_config.unlocks < 2 && tableOffset == eng::kKit_WeaponTable)
            continue;
        void* table = nullptr;
        void* parts = nullptr;
        if (!eng::ReadPtr(At(kit, tableOffset), &table) || !table ||
            !eng::ReadPtr(At(table, eng::kCustomizationTable_UnlockParts), &parts))
            continue;
        for (uint32_t i = 0, n = ArrayCount(parts); i < n; ++i)
        {
            void* part = ArrayAt(parts, i);
            void* automatic = nullptr;
            void* selectable = nullptr;
            if (!part)
                continue;
            if (eng::ReadPtr(At(part, eng::kUnlockParts_Automatic), &automatic))
                for (uint32_t k = 0, m = ArrayCount(automatic); k < m; ++k)
                    add(ArrayAt(automatic, k));
            if (eng::ReadPtr(At(part, eng::kUnlockParts_Selectable), &selectable) && ArrayCount(selectable))
                add(ArrayAt(selectable, 0));
        }
    }
    Log("  [%s] default unlocks from kit %d: %d (%s); not UnlockAsset, skipped: %d (%s)", bot.name, kitIndex, added,
        names, skipped, skippedNames);
    return added;
}

bool SpawnInFrontOf(Bot& bot, void* hostSoldier)
{
    const float* host = nullptr;
    if (!eng::GetSoldierTransform(hostSoldier, &host) || !host)
        return false;
    alignas(16) float t[16];
    std::memcpy(t, host, sizeof(t));
    for (int i = 0; i < 3; ++i)
        t[12 + i] += t[8 + i] * 3.0f;  // trans += forward * 3 m
    Log("  [%s] spawnAt (%.1f, %.1f, %.1f)", bot.name, t[12], t[13], t[14]);
    return eng::SpawnAt(bot.player, t);
}

std::vector<brain::Actor> Actors(void* context);

float Distance(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

constexpr uint32_t kSightFlags = eng::kRay_DontCheckWater | eng::kRay_DontCheckRagdoll | eng::kRay_DontCheckCharacter;

// The game's "buddy in combat" test (SquadSpawnFailReason CombatSuppressed / CombatProjectile / CombatStrike) runs on
// the buddy's soldier; this approximates it from what the DLL knows: the buddy fought within its data's
// CantSpawnOnBuddyCombatTime (a bot: shooting, or a target in sight), an enemy is within BuddySafeMeters, or one of the
// nearest enemies within 100 m has a line of sight to it. Returns the reason, or null when it's safe.
const char* BuddyCombatReason(void* buddy, void* buddySoldier, const float buddyPos[3], int team,
                              const std::vector<brain::Actor>& actors, char* reason, size_t size)
{
    float combatTime = 5.0f;
    void* data = nullptr;
    float configured = 0.0f;
    if (eng::ReadPtr(At(buddySoldier, eng::kEntity_Data), &data) && data &&
        eng::ReadFloat(At(data, eng::kControllableData_CantSpawnOnBuddyCombatTime), &configured) && configured > 0.0f &&
        configured < 60.0f)
        combatTime = configured;
    if (const Bot* b = FindBot(buddy))
        if (brain::InCombat(b->brain, combatTime))
        {
            std::snprintf(reason, size, "fighting in the last %.0f s", combatTime);
            return reason;
        }
    std::vector<std::pair<float, const brain::Actor*>> enemies;
    for (const brain::Actor& a : actors)
        if (a.team != team && a.team != 0)
            enemies.push_back({Distance(buddyPos, a.pos), &a});
    std::sort(enemies.begin(), enemies.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    char name[24];
    if (!enemies.empty() && enemies[0].first < static_cast<float>(g_config.buddySafeMeters))
    {
        std::snprintf(reason, size, "%s %.0f m away", NameOf(enemies[0].second->player, name, sizeof(name)),
                      enemies[0].first);
        return reason;
    }
    float chest[3] = {buddyPos[0], buddyPos[1] + 1.1f, buddyPos[2]};
    for (size_t i = 0; i < enemies.size() && i < 4 && enemies[i].first < 100.0f; ++i)
    {
        const float* e = enemies[i].second->pos;
        float eye[3] = {e[0], e[1] + 1.6f, e[2]};
        bool blocked = true;
        if (eng::RayBlocked(eye, chest, kSightFlags, &blocked) && !blocked)
        {
            std::snprintf(reason, size, "in sight of %s at %.0f m",
                          NameOf(enemies[i].second->player, name, sizeof(name)), enemies[i].first);
            return reason;
        }
    }
    return nullptr;
}

// Next to the fireteam buddy (the game's own pairing) when it is alive, out of combat and not swimming: 1.5 m behind
// it, else beside it, where there is no wall in between, ground underfoot and no water over it. False: fall back.
bool TrySpawnOnBuddy(void* context, Bot& bot)
{
    void* buddy = nullptr;
    void* buddysBuddy = nullptr;
    if (!eng::GetFireteamBuddy(bot.player, &buddy) || !buddy)
    {
        Log("  [%s] no fireteam buddy; fall-back spawn", bot.name);
        return false;
    }
    // Only a mutual pairing: a player not yet in a fireteam would otherwise match any teammate.
    char name[24];
    const char* buddyName = NameOf(buddy, name, sizeof(name));
    // BF3's vars.onlySquadLeaderSpawn (Hardcore): the game's own spawn screen asks this for you (0xF614FF).
    bool buddyLeads = false;
    if (hardcore::LeaderSpawnOnly() && eng::IsSquadLeader(buddy, &buddyLeads) && !buddyLeads)
    {
        Log("  [%s] only the fireteam leader can be spawned on, and %s isn't; fall-back spawn", bot.name, buddyName);
        return false;
    }
    if (!eng::GetFireteamBuddy(buddy, &buddysBuddy) || buddysBuddy != bot.player)
    {
        Log("  [%s] fireteam pairing with %s isn't mutual yet; fall-back spawn", bot.name, buddyName);
        return false;
    }
    void* buddySoldier = SoldierOf(buddy);
    const float* bt = nullptr;
    if (!buddySoldier || !eng::GetSoldierTransform(buddySoldier, &bt) || !bt)
    {
        Log("  [%s] buddy %s is dead; fall-back spawn", bot.name, buddyName);
        return false;
    }
    float buddyPos[3] = {bt[12], bt[13], bt[14]};
    int buddyState = eng::kStateOnGround;
    if (eng::GetCharacterPose(buddySoldier, nullptr, nullptr, &buddyState) && buddyState == eng::kStateSwimming)
    {
        Log("  [%s] buddy %s is swimming; fall-back spawn", bot.name, buddyName);
        return false;
    }
    // A bot buddy that's stuck - in a hole it can't climb out of, say (Drumstick on Novi Grad): not there too.
    if (const Bot* b = FindBot(buddy); b && b->brain.stuckStreak >= 2)
    {
        Log("  [%s] buddy %s is stuck; fall-back spawn", bot.name, buddyName);
        return false;
    }
    char reason[96];
    if (const char* why = BuddyCombatReason(buddy, buddySoldier, buddyPos, bot.team, Actors(context), reason,
                                            sizeof(reason)))
    {
        Log("  [%s] buddy %s is in combat (%s); fall-back spawn", bot.name, buddyName, why);
        return false;
    }
    alignas(16) float t[16];
    std::memcpy(t, bt, sizeof(t));
    const float offsets[3][2] = {{0.0f, -1.5f}, {-1.2f, -0.5f}, {1.2f, -0.5f}};  // (right, forward) metres
    for (const auto& o : offsets)
    {
        float pos[3] = {buddyPos[0] + bt[0] * o[0] + bt[8] * o[1], buddyPos[1], buddyPos[2] + bt[2] * o[0] + bt[10] * o[1]};
        float buddyChest[3] = {buddyPos[0], buddyPos[1] + 1.1f, buddyPos[2]};
        float chest[3] = {pos[0], pos[1] + 1.1f, pos[2]};
        float below[3] = {pos[0], pos[1] - 0.6f, pos[2]};
        bool wall = true;
        bool ground = false;
        if (!eng::RayBlocked(buddyChest, chest, kSightFlags, &wall) || wall ||
            !eng::RayBlocked(chest, below, kSightFlags, &ground) || !ground)
            continue;
        if (eng::WaterAt(pos, 0.5f))  // the buddy at the water's edge: not a spot in the water behind it
            continue;
        t[12] = pos[0];
        t[13] = pos[1];
        t[14] = pos[2];
        bool ok = eng::SpawnAt(bot.player, t);
        Log("  [%s] spawn on fireteam buddy %s at (%.1f, %.1f, %.1f): %s", bot.name, buddyName, pos[0], pos[1], pos[2],
            ok ? "ok" : "FAILED");
        return ok && SoldierOf(bot.player);
    }
    Log("  [%s] no room next to buddy %s; fall-back spawn", bot.name, buddyName);
    return false;
}

// Fall-back spawns in the bot's own base. MP_03 TDM's tactical spawn-point chooser has TeamFilter off, so any of its
// 49 points was fair game - the shared ones mid-map and the other team's base too: enemy bots spawned in your base
// and yours in theirs. While a bot's fall-back spawn runs, the chooser's TeamFilter getter (0x26BC090) reads true and
// the AlternateSpawnEntityData.Team getter (0x25D0770) turns a shared point's team 0 into nobody's, so the chooser's
// filter (0xEE1C90: keeps a point whose team is 0 or the player's) keeps only the bot's own team's points.
// Both getters are property reads, thiscall(out, context) ret 8, filling out = {override, &field}; the value is
// *(*override & ~3) when *override has bit 0 set, else *field (0x564260). The detours point out at their own value.
using PropertyGetFn = void**(__thiscall*)(void* data, void** out, void* context);
PropertyGetFn g_getTeamFilter = nullptr;
PropertyGetFn g_getPointTeam = nullptr;
DWORD g_ownBaseThread = 0;  // non-zero while a bot's fall-back spawn runs, on this thread
int g_ownBaseTeam = 0;
bool g_sharedOnly = false;  // this spawn is from the shared points instead: the bot's base is camped
constexpr float kBaseRadius = 30.0f;
constexpr int kPointData_Position = 0x40;  // AlternateSpawnEntityData is a SpatialEntityData: Transform at 0x10, its position 0x30 in
struct Position
{
    float v[3];
};
std::vector<Position> g_spawnAvoid;       // this shared spawn: where enemies are...
Position g_spawnOwnBase = {};             // ...and the base it's leaving
std::vector<Position> g_sharedPointsSeen;  // the shared points this spawn looked at
int g_sharedPointsSkipped = 0;
int g_pointPositionMisses = 0;            // landings far from every position read: the positions aren't world positions
constexpr int kPointPositionMissLimit = 3;
int g_ownBasePoints[3] = {};  // points the chooser looked at: the bot's team's, shared, the other team's
std::vector<void*> g_ownBaseSeen;  // their data (the filter reads a point's team twice)
const bool kTeamFilterOn = true;
const int kNobodysTeam = 99;

bool OwnBaseSpawnRunning() { return g_ownBaseThread && g_ownBaseThread == GetCurrentThreadId(); }

void** __fastcall TeamFilterDetour(void* data, void* /*edx*/, void** out, void* context)
{
    void** result = g_getTeamFilter(data, out, context);
    if (OwnBaseSpawnRunning())
    {
        out[0] = nullptr;
        out[1] = const_cast<bool*>(&kTeamFilterOn);
    }
    return result;
}

// A shared point too near an enemy or inside the camped base: the chooser never sees it. Its own test only drops points an
// enemy can see, and on Basilan that left bots appearing under 10 m from one in 38 of 134 shared spawns.
bool SharedPointUnsafe(void* data, bool firstLook)
{
    Position at = {};
    if (!eng::ReadBytes(eng::At(data, kPointData_Position), at.v, sizeof(at.v)))
        return false;
    if (firstLook)
        g_sharedPointsSeen.push_back(at);
    if (g_pointPositionMisses >= kPointPositionMissLimit)
        return false;
    bool unsafe = Distance(at.v, g_spawnOwnBase.v) < kBaseRadius;
    for (const Position& enemy : g_spawnAvoid)
        unsafe = unsafe || Distance(at.v, enemy.v) < static_cast<float>(g_config.sharedSpawnSafeMeters);
    if (unsafe && firstLook)
        ++g_sharedPointsSkipped;
    return unsafe;
}

void** __fastcall PointTeamDetour(void* data, void* /*edx*/, void** out, void* context)
{
    void** result = g_getPointTeam(data, out, context);
    if (OwnBaseSpawnRunning())
    {
        const void* value = out[1];
        void* tagged = nullptr;
        if (out[0] && eng::ReadPtr(out[0], &tagged) && tagged && (reinterpret_cast<uintptr_t>(tagged) & 1))
            value = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(tagged) & ~uintptr_t(3));
        uint32_t team = 0;
        if (value && eng::ReadU32(value, &team))
        {
            const bool firstLook = std::find(g_ownBaseSeen.begin(), g_ownBaseSeen.end(), data) == g_ownBaseSeen.end();
            if (firstLook)
            {
                g_ownBaseSeen.push_back(data);
                ++g_ownBasePoints[team == static_cast<uint32_t>(g_ownBaseTeam) ? 0 : team == 0 ? 1 : 2];
            }
            bool unsafePoint = false;
            if (g_sharedOnly && team == 0)
                unsafePoint = SharedPointUnsafe(data, firstLook);
            if (unsafePoint || (g_sharedOnly ? team == static_cast<uint32_t>(g_ownBaseTeam) : team == 0))
            {
                out[0] = nullptr;
                out[1] = const_cast<int*>(&kNobodysTeam);
            }
        }
    }
    return result;
}

void InstallOwnBaseSpawnHooks()
{
    if (!g_config.fallBackToOwnBase)
        return;
    if (void* filter = reinterpret_cast<void*>(eng::Address("TacticalChooser_getTeamFilter")))
        g_getTeamFilter = reinterpret_cast<PropertyGetFn>(
            InlineHook(filter, 9, reinterpret_cast<const void*>(&TeamFilterDetour)));
    if (void* team = reinterpret_cast<void*>(eng::Address("AlternateSpawn_getTeam")))
        g_getPointTeam = reinterpret_cast<PropertyGetFn>(
            InlineHook(team, 6, reinterpret_cast<const void*>(&PointTeamDetour)));
    if (g_config.sharedSpawnWhenCamped)
        Log("spawns: a camped base (%d enemies within %.0f m for %d s, or %d players killed there soon after spawning in 30 s) sends "
            "its bots to shared points at least %d m from any enemy", g_config.campedEnemies, kBaseRadius, g_config.campedSeconds,
            g_config.campedSpawnDeaths, g_config.sharedSpawnSafeMeters);
    Log("spawns: bots falling back spawn in their own base %s%s",
        g_getTeamFilter && g_getPointTeam ? "(own team's spawn points only)" : "- hook FAILED (the game's choice)",
        modes::IsTeamDeathmatch() ? "" : "; not in this mode, whose own spawn rules decide");
}

// MOHW deletes an AI player as soon as its soldier dies or unspawns - the campaign's rule for enemy soldiers; a human
// keeps theirs and respawns. Three soldier paths (0xB61BC0, 0xB6BE10, 0xB18AD0) clear the player's soldier, then
// `cmp byte [player+0x354], 0; je skip` -> release its loadout (0xB42D40) and ServerPlayerManager::deletePlayer
// (0xB3AC90). So every bot death made a new player: a new id, no scoreboard history, and your game deleted the
// ClientPlayer the falling body belonged to. Each site's 9 bytes become `call stub; je skip; nop; nop`: the stub
// pushes the player's register, asks this, and sets ZF from the answer (ZF = keep the player), keeping ecx and edx.
bool __cdecl ShouldDeleteAiPlayer(void* player)
{
    uint8_t ai = 0;
    if (!eng::ProbeByte(At(player, eng::kPlayer_IsAI), &ai) || !ai)
        return false;  // the game's own test: only AI players go
    if (std::find(g_kicked.begin(), g_kicked.end(), player) != g_kicked.end())
        return true;
    if (levelchange::Pending())
        return true;  // leaving for an in-place level change: no bot player may live into the next level
    return !(g_config.keepPlayers && FindBot(player));
}

bool PatchAiDeathSite(const char* sigName, uint8_t pushPlayer)
{
    auto* site = reinterpret_cast<uint8_t*>(eng::Address(sigName));
    if (!site || site[7] != 0x74 || static_cast<int8_t>(site[8]) < 0 || site[8] > 0x7D)
        return false;
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub)
        return false;
    auto rel32 = [](const uint8_t* next, const void* target) {
        return static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(next));
    };
    uint8_t* p = stub;
    *p++ = 0x51;        // push ecx
    *p++ = 0x52;        // push edx
    *p++ = pushPlayer;  // push edi / esi: the player
    *p++ = 0xE8;        // call ShouldDeleteAiPlayer
    int32_t call = rel32(p + 4, reinterpret_cast<const void*>(&ShouldDeleteAiPlayer));
    std::memcpy(p, &call, 4);
    p += 4;
    *p++ = 0x83;  // add esp, 4
    *p++ = 0xC4;
    *p++ = 0x04;
    *p++ = 0x5A;  // pop edx
    *p++ = 0x59;  // pop ecx
    *p++ = 0x84;  // test al, al
    *p++ = 0xC0;
    *p++ = 0xC3;  // ret
    FlushInstructionCache(GetCurrentProcess(), stub, p - stub);

    uint8_t skip = site[8];  // je rel8 from site + 9; the new je sits 2 bytes earlier
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    site[0] = 0xE8;
    int32_t toStub = rel32(site + 5, stub);
    std::memcpy(site + 1, &toStub, 4);
    site[5] = 0x74;
    site[6] = static_cast<uint8_t>(skip + 2);
    site[7] = 0x90;
    site[8] = 0x90;
    VirtualProtect(site, 9, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, 9);
    return true;
}

void InstallKeepPlayers()
{
    if (!g_config.keepPlayers)
        return;
    bool killed = PatchAiDeathSite("AiDeath_deletePlayer1", 0x57);    // push edi
    bool unspawned = PatchAiDeathSite("AiDeath_deletePlayer2", 0x57);  // push edi
    bool removed = PatchAiDeathSite("AiDeath_deletePlayer3", 0x56);    // push esi
    if (!(killed && unspawned && removed))
        g_config.keepPlayers = 0;  // a bot whose player goes anyway comes back as a new one, as before
    Log("players: a dead bot keeps its player and respawns as itself (name, team, fireteam, scoreboard row)%s",
        g_config.keepPlayers ? "" : " - patch FAILED; dead bots come back as new players");
}

// MOHW's spawn rule, as for human players: on the fireteam buddy unless it is in combat, else "falling back" to a
// spawn point the engine's tactical chooser picks away from enemy sight (ServerPlayerForceRespawnMessage) - one of
// the bot's own team's (above). No free point there: the chooser's own pick.
// A camped base: where each team's own-base spawns land (the base is about 30 m across on the maps played so far) and who
// has died there soon after spawning. Enough enemies staying near it, or enough of those deaths, makes it camped until 15 s
// after neither is true; its bots then spawn from the map's shared points, skipping any near an enemy. Early in a round the
// level has no shared points (it offers 10 / 0 / 10, later 5 / 25 / 5), and then the bot uses its own base after all.
// First version: 2 enemies at the moment of a spawn was enough. On Basilan (bases 77 m apart) both bases then stayed camped
// from minute 4 to the end, and 32% of those spawns died within 15 s against 6% from the bases.
struct BaseWatch
{
    float sum[3] = {};
    int spawns = 0;
    std::vector<DWORD> spawnDeaths;
    float occupied = 0.0f;  // seconds enemies have been near it, falling again when they aren't
    DWORD campedUntil = 0;
    bool camped = false;
};
BaseWatch g_bases[3];  // by team id
constexpr float kSpawnDeathAge = 15.0f;
constexpr DWORD kSpawnDeathWindowMs = 30000;
constexpr DWORD kCampedHoldMs = 15000;

BaseWatch* WatchFor(int team) { return team == 1 || team == 2 ? &g_bases[team] : nullptr; }

bool BaseCenter(const BaseWatch& base, float out[3])
{
    if (!base.spawns)
        return false;
    for (int i = 0; i < 3; ++i)
        out[i] = base.sum[i] / static_cast<float>(base.spawns);
    return true;
}

void NoteBaseSpawn(int team, void* soldier)
{
    BaseWatch* base = WatchFor(team);
    const float* t = nullptr;
    if (!base || !soldier || !eng::GetSoldierTransform(soldier, &t) || !t)
        return;
    for (int i = 0; i < 3; ++i)
        base->sum[i] += t[12 + i];
    ++base->spawns;
}

// A player killed by the other team: counts if it had only just spawned, and in its base.
void NoteDeath(const scoring::Kill& kill, const brain::Actor* victim)
{
    BaseWatch* base = WatchFor(kill.victimTeam);
    float center[3];
    if (!base || !victim || !kill.killer || kill.killerTeam == kill.victimTeam || victim->spawnAge >= kSpawnDeathAge ||
        !BaseCenter(*base, center) || Distance(victim->pos, center) >= kBaseRadius)
        return;
    base->spawnDeaths.push_back(GetTickCount());
}

// Every tick, server thread.
void UpdateBases(const std::vector<brain::Actor>& actors, float dt)
{
    if (!g_config.sharedSpawnWhenCamped || !modes::IsTeamDeathmatch())
        return;
    const DWORD now = GetTickCount();
    for (int team = 1; team <= 2; ++team)
    {
        BaseWatch& base = g_bases[team];
        float center[3];
        if (!BaseCenter(base, center))
            continue;
        base.spawnDeaths.erase(std::remove_if(base.spawnDeaths.begin(), base.spawnDeaths.end(),
                                              [now](DWORD at) { return now - at > kSpawnDeathWindowMs; }),
                               base.spawnDeaths.end());
        int nearby = 0;
        for (const brain::Actor& a : actors)
            if ((a.team == 1 || a.team == 2) && a.team != team && Distance(a.pos, center) < kBaseRadius)
                ++nearby;
        if (g_config.campedEnemies > 0 && nearby >= g_config.campedEnemies)
            base.occupied = std::fmin(base.occupied + dt, static_cast<float>(g_config.campedSeconds) * 2.0f);
        else
            base.occupied = std::fmax(base.occupied - dt, 0.0f);
        const int deaths = static_cast<int>(base.spawnDeaths.size());
        const bool enemies = g_config.campedEnemies > 0 && base.occupied >= static_cast<float>(g_config.campedSeconds);
        const bool killed = g_config.campedSpawnDeaths > 0 && deaths >= g_config.campedSpawnDeaths;
        if (enemies || killed)
            base.campedUntil = now + kCampedHoldMs;
        const bool camped = static_cast<LONG>(base.campedUntil - now) > 0;
        if (camped == base.camped)
            continue;
        base.camped = camped;
        if (camped)
            Log("spawns: team %d's base is camped (%d enemies within %.0f m now, %.0f s of them lately, %d players killed there soon "
                "after spawning in the last 30 s): its bots use the shared points", team, nearby, kBaseRadius, base.occupied, deaths);
        else
            Log("spawns: team %d's base is clear again: its bots use it again", team);
    }
}

bool BaseCamped(int team)
{
    const BaseWatch* base = WatchFor(team);
    return base && base->camped;
}

float NearestEnemy(void* context, int team, const float pos[3])
{
    float nearest = -1.0f;
    for (const brain::Actor& a : Actors(context))
        if ((a.team == 1 || a.team == 2) && a.team != team)
        {
            float d = Distance(a.pos, pos);
            if (nearest < 0.0f || d < nearest)
                nearest = d;
        }
    return nearest;
}

bool SpawnBotOnce(void* context, Bot& bot)
{
    if (g_config.spawnOnBuddy && TrySpawnOnBuddy(context, bot))
        return true;
    if (g_getTeamFilter && g_getPointTeam && modes::IsTeamDeathmatch())
    {
        g_ownBaseTeam = bot.team;
        if (BaseCamped(bot.team))
        {
            g_ownBasePoints[0] = g_ownBasePoints[1] = g_ownBasePoints[2] = 0;
            g_ownBaseSeen.clear();
            g_sharedPointsSeen.clear();
            g_sharedPointsSkipped = 0;
            g_spawnAvoid.clear();
            for (const brain::Actor& a : Actors(context))
                if ((a.team == 1 || a.team == 2) && a.team != bot.team)
                    g_spawnAvoid.push_back({{a.pos[0], a.pos[1], a.pos[2]}});
            BaseCenter(*WatchFor(bot.team), g_spawnOwnBase.v);
            g_sharedOnly = true;
            g_ownBaseThread = GetCurrentThreadId();
            bool shared = eng::ForceRespawn(context, bot.player);
            g_ownBaseThread = 0;
            g_sharedOnly = false;
            if (void* soldier = SoldierOf(bot.player))
            {
                const float* t = nullptr;
                float at[3] = {};
                if (eng::GetSoldierTransform(soldier, &t) && t)
                    std::memcpy(at, t + 12, sizeof(at));
                // The points' stored positions should be world positions; a landing far from all of them says they aren't.
                float gap = -1.0f;
                for (const Position& point : g_sharedPointsSeen)
                {
                    float d = Distance(point.v, at);
                    if (gap < 0.0f || d < gap)
                        gap = d;
                }
                if (gap > 5.0f && ++g_pointPositionMisses == kPointPositionMissLimit)
                    Log("spawns: spawn point positions don't match where bots land; shared points are no longer skipped for being near enemies");
                Log("  [%s] its base is camped: shared spawn at (%.1f, %.1f, %.1f), nearest enemy %.0f m, %.1f m from the point's stored "
                    "position; %d of %d shared points skipped as unsafe", bot.name, at[0], at[1], at[2], NearestEnemy(context, bot.team, at),
                    gap, g_sharedPointsSkipped, g_ownBasePoints[1]);
                return shared;
            }
            Log("  [%s] its base is camped but no shared point is safe (%d of %d skipped); its own base", bot.name, g_sharedPointsSkipped, g_ownBasePoints[1]);
        }
        g_ownBasePoints[0] = g_ownBasePoints[1] = g_ownBasePoints[2] = 0;
        g_ownBaseSeen.clear();
        g_ownBaseThread = GetCurrentThreadId();
        bool ok = eng::ForceRespawn(context, bot.player);
        g_ownBaseThread = 0;
        if (SoldierOf(bot.player))
        {
            NoteBaseSpawn(bot.team, SoldierOf(bot.player));
            Log("  [%s] fall-back spawn from its own team's points only (the chooser looked at team %d's %d, shared "
                "%d, the other team's %d)", bot.name, bot.team, g_ownBasePoints[0], g_ownBasePoints[1],
                g_ownBasePoints[2]);
            return ok;
        }
        Log("  [%s] no free spawn point in its own base (team %d points %d); the game's pick instead", bot.name,
            bot.team, g_ownBasePoints[0]);
    }
    return eng::ForceRespawn(context, bot.player);
}

// False only when the spawn faulted inside the game (logged by eng::ForceRespawn): counted for the level (see
// g_spawnFaults).
bool SpawnBot(void* context, Bot& bot)
{
    if (SpawnBotOnce(context, bot))
        return true;
    ++g_spawnFaults;
    Log("bot %s: its spawn faulted inside the game (%d on this level)", bot.name, g_spawnFaults);
    if (g_spawnFaults >= kSpawnFaultLimit && !g_spawnsHalted)
    {
        g_spawnsHalted = true;
        Log("bots: spawns keep faulting on this level; no more bot spawns here (each faulted spawn leaves a half-made "
            "soldier behind, and enough of them froze the game)");
        levelchange::RestartAfterSpawnFaults();  // a level loaded in place: the game restarts with it
        rcon::Announce("Bots can't spawn on this level; use Next Round or Restart Round to load one again.");
    }
    return false;
}

// respawn: re-add a dead bot (its team and name, buddy or fall-back spawn, no beep). automatic: the team fill (no beep).
void AddBot(void* context, bool atMe, bool friendly, const PendingRespawn* respawn = nullptr, bool automatic = false)
{
    void* manager = PlayerManager(context);
    if (!manager)
    {
        Log("add bot: no ServerPlayerManager yet");
        return;
    }
    if (g_spawnsHalted)
    {
        Log("add bot: bot spawns are stopped on this level (they faulted inside the game)");
        KeyFeedback(automatic ? &kSilent : respawn, false);
        return;
    }
    std::vector<void*> players = Players(manager);
    void* host = FindHost(players);
    uint32_t hostTeam = 0;
    if (host)
        eng::ReadU32(At(host, eng::kPlayer_Team), &hostTeam);
    void* hostSoldier = host ? SoldierOf(host) : nullptr;
    if (atMe && !hostSoldier)
    {
        Log("add bot (at me): your soldier isn't spawned; spawn first or use the other key");
        KeyFeedback(automatic ? &kSilent : respawn, false);
        return;
    }

    Bot bot;
    if (respawn)
        bot.team = respawn->team;
    else if (friendly)
        bot.team = hostTeam ? static_cast<int>(hostTeam) : (g_config.humanTeam > 0 ? g_config.humanTeam : 1);
    else
        bot.team = g_config.team ? g_config.team : (hostTeam == 1 ? 2 : 1);
    if (respawn)
        std::snprintf(bot.name, sizeof(bot.name), "%s", respawn->name);
    else
        std::snprintf(bot.name, sizeof(bot.name), "%s", PickName().c_str());
    Log("add bot %s: players=%u host=%p hostTeam=%u -> team %d (%s, %s)", bot.name,
        static_cast<unsigned>(players.size()), host, hostTeam, bot.team,
        respawn ? "respawn" : friendly ? "teammate" : "enemy", atMe ? "spawn in front of host" : "buddy or fall-back spawn");

    uint32_t teamCount = 0;
    void* teamData = TeamDataFor(context, bot.team, &teamCount);
    int kitCount = -1;
    void* blueprint = nullptr;
    if (teamData)
        blueprint = ChooseBlueprint(teamData, &kitCount);
    else if (hostSoldier && eng::ReadPtr(At(hostSoldier, eng::kEntity_Data), &blueprint) && blueprint)
        // Campaign levels have no TeamEntityData; instantiate the host soldier's own data instead.
        Log("  no TeamData for team %d (team count %u); using your soldier's data %p as the asset", bot.team, teamCount,
            blueprint);
    if (!blueprint)
    {
        Log("  no TeamData for team %d (team count %u) and no soldier of yours to copy; aborting", bot.team, teamCount);
        KeyFeedback(automatic ? &kSilent : respawn, false);
        return;
    }

    if (!eng::CreatePlayer(manager, &bot.player) || !bot.player)
    {
        Log("  createPlayer returned null (server full?)");
        KeyFeedback(automatic ? &kSilent : respawn, false);
        return;
    }
    Log("  [%s] ServerPlayer %p", bot.name, bot.player);
    eng::SetPlayerName(manager, bot.player, bot.name);
    eng::SetIsAIPlayer(bot.player, true);
    scoreboard::AddBot(bot.player);  // its row is made as it joins the team
    eng::SetTeam(bot.player, bot.team);
    eng::SetSelectedSoldierAsset(bot.player, blueprint);

    // KitIndex -1 = a random kit per bot (a mixed team, as fun-bots spawns them).
    int kitIndex = g_config.kitIndex;
    if (kitIndex < 0)
        kitIndex = kitCount > 0 ? static_cast<int>(NextRandom() % static_cast<uint32_t>(kitCount)) : 0;
    else if (kitCount >= 0 && kitIndex >= kitCount)
        kitIndex = 0;
    void* loadout = nullptr;
    if (eng::GetPlayerLoadoutState(bot.player, &loadout) && loadout)
    {
        eng::SetKitIndex(loadout, kitIndex);
        Log("  [%s] loadout state %p, kit index %d of %d", bot.name, loadout, kitIndex, kitCount);
    }
    else
        Log("  [%s] no loadout state for this player; kit index left at default", bot.name);
    if (void* kitAsset = teamData ? KitFor(teamData, kitIndex) : nullptr)
    {
        uint32_t type = 0;
        if (eng::ReadU32(At(kitAsset, eng::kKit_MPKitType), &type))
            bot.kitType = static_cast<int>(type);
    }
    if (void* kit = teamData && g_config.unlocks >= 2 ? KitFor(teamData, kitIndex) : nullptr)
    {
        // The game's own path for a player whose client sent no choices: kit, weapons, appearance.
        char kitName[64];
        ShortAssetName(kit, kitName, sizeof(kitName));
        PartSwaps swapped = BorrowSupportGrenades(bot, teamData, kit);
        bool ok = eng::ApplyDefaultLoadout(bot.player, kit);
        RestoreParts(swapped);
        Log("  [%s] game default loadout for %s: %s", bot.name, kitName, ok ? "applied" : "FAILED");
        LogPlayerUnlocks(bot.player, "    unlocks after it");
    }
    // If that left no appearance (or Unlocks=1), the kit's own kit + appearance unlocks; these alone made bots spawn.
    if (teamData && g_config.unlocks >= 1 && UnlockCount(bot.player) == 0)
        GiveDefaultUnlocks(bot, teamData, kitIndex);

    bot.input = _aligned_malloc(eng::kEntryInputAlloc, 16);
    std::memset(bot.input, 0, eng::kEntryInputAlloc);
    if (!eng::ConstructEntryInput(bot.input) || !eng::SetAuthoritativeAiming(bot.input, true) ||
        !eng::SetInput(bot.player, bot.input))
        Log("  [%s] WARNING: input setup failed", bot.name);

    g_bots.push_back(bot);
    Bot& added = g_bots.back();
    g_botHistory[added.player] = {added.name, added.kitType};
    bool spawned = atMe ? SpawnInFrontOf(added, hostSoldier) : SpawnBot(context, added);
    void* soldier = SoldierOf(added.player);
    Log("  [%s] spawn call %s; soldier now %p", added.name, spawned ? "returned" : "FAILED", soldier);
    KeyFeedback(automatic ? &kSilent : respawn, spawned && soldier);
}

void ReleaseBot(Bot& bot, bool playerStillValid)
{
    if (playerStillValid)
        eng::SetInput(bot.player, nullptr);
    if (bot.input)
    {
        eng::DestroyEntryInput(bot.input);
        _aligned_free(bot.input);
    }
    bot.input = nullptr;
}

// Takes g_bots[i] off every list that knows its player (its input released already): the player is gone, and a new
// player may be made at the same address.
void Forget(size_t i)
{
    void* player = g_bots[i].player;
    nav::ForgetWalker(player);
    scoreboard::ForgetBot(player);
    g_kicked.erase(std::remove(g_kicked.begin(), g_kicked.end(), player), g_kicked.end());
    g_bots.erase(g_bots.begin() + static_cast<std::ptrdiff_t>(i));
}

// Drops bots whose player no longer exists (level change, kick) or no longer uses our input.
void ValidateBots(void* context)
{
    std::vector<void*> players = context ? Players(PlayerManager(context)) : std::vector<void*>{};
    for (size_t i = 0; i < g_bots.size();)
    {
        Bot& bot = g_bots[i];
        bool present = false;
        for (void* p : players)
            present |= p == bot.player;
        void* input = nullptr;
        bool ours = present && eng::ReadPtr(At(bot.player, eng::kPlayer_Input), &input) && input == bot.input;
        if (ours)
        {
            ++i;
            continue;
        }
        if (!present && context && g_config.autoRespawnSeconds > 0)
        {
            // Died: the engine destroyed its player. Bring it back as a new player with the same name and team.
            PendingRespawn respawn;
            respawn.team = bot.team;
            respawn.round = levelchange::Pending() ? -1 : RoundOfDeath();
            std::snprintf(respawn.name, sizeof(respawn.name), "%s", bot.name);
            g_respawns.push_back(respawn);
            if (levelchange::Pending())
                Log("bot %s: left for the level change (its player went with its soldier); back as a new player %d s "
                    "after the new level is in", bot.name, g_config.autoRespawnSeconds);
            else
                Log("bot %s: died (its player was destroyed); respawning in %d s", bot.name, g_config.autoRespawnSeconds);
        }
        else
            Log("bot %s: player %p gone or replaced; releasing its input", bot.name, bot.player);
        ReleaseBot(bot, present);
        Forget(i);
    }
}

// The first part of an in-place level change (levelchange.h): every bot leaves the game. A bot's player holds this level's
// soldier blueprint, kit and unlock assets, which the reload frees; respawned from them after the first change, every
// soldier came out without physics and faulted inside the game, until the game froze. A living bot's soldier is killed
// and the game's own path for a dead AI player deletes the player (ShouldDeleteAiPlayer says yes during a change); a dead
// bot's player is deleted here the same way. Each comes back with its name and team as a new player on the new level.
// Runs last in the tick: a player deleted by the kill must not be looked at again before ValidateBots drops it.
void ReleaseForLevelChange(void* context)
{
    g_spawnFaults = 0;  // the next level is a new start
    g_spawnsHalted = false;
    for (size_t i = 0; i < g_bots.size();)
    {
        Bot& bot = g_bots[i];
        if (bot.deleteFailed)
        {
            ++i;  // left alone
            continue;
        }
        // Killed on an earlier tick and still listed: ValidateBots found its player this tick, so it can be looked at.
        if (void* soldier = SoldierOf(bot.player))
        {
            if (!bot.killedToLeave)
            {
                bot.killedToLeave = true;
                if (!eng::KillSoldier(soldier))
                    Log("bot %s: its soldier %p couldn't be killed for the level change", bot.name, soldier);
            }
            ++i;  // not looked at again in this tick: the kill may have deleted its player
            continue;
        }
        ReleaseBot(bot, true);
        if (!eng::DeleteAiPlayer(context, bot.player))
        {
            Log("bot %s: deleting its player %p for the level change FAILED", bot.name, bot.player);
            bot.deleteFailed = true;
            ++i;
            continue;
        }
        if (g_config.autoRespawnSeconds > 0)
        {
            PendingRespawn respawn;
            respawn.team = bot.team;
            respawn.round = -1;
            std::snprintf(respawn.name, sizeof(respawn.name), "%s", bot.name);
            g_respawns.push_back(respawn);
        }
        Log("bot %s: player deleted for the level change (it was dead); back as a new player %d s after the new level is in",
            bot.name, g_config.autoRespawnSeconds);
        Forget(i);
    }
}

void Drive(Bot& bot, void* context, const std::vector<brain::Actor>& actors, float dt)
{
    if (bot.deleteFailed)
        return;  // its player couldn't be deleted for a level change (input released): left alone
    if (bot.killedToLeave)
    {
        if (levelchange::Pending())
            return;  // leaving for a level change: its player may be gone already
        bot.killedToLeave = false;  // the change was given up and the game plays on here: so does the bot
    }
    void* soldier = SoldierOf(bot.player);
    if (!soldier)
    {
        if (bot.hadSoldier)
        {
            bot.diedInRound = RoundOfDeath();
            bot.toldOut = false;
            Log("bot %s: died; respawning in %d s (same player)%s", bot.name, g_config.autoRespawnSeconds,
                SitsOutRound(bot.diedInRound) ? " once the next round starts (one life per round)" : "");
        }
        bot.hadSoldier = false;
        bot.deadSeconds += dt;
        if (StandDown() || g_spawnsHalted)
            return;
        if (SitsOutRound(bot.diedInRound))
        {
            bot.deadSeconds = 0.0f;
            return;
        }
        if (g_config.autoRespawnSeconds > 0 && bot.deadSeconds >= g_config.autoRespawnSeconds)
        {
            // No level (between levels, or the server going down after you left): a spawn now faults inside the game
            // (the server's world table at 0x2A4F724 is empty; 0x4CAD41) and left a half-made soldier as the server shut
            // down, after which the game hung. It waits for a level with its team in it.
            if (!TeamDataFor(context, bot.team, nullptr))
            {
                if (!bot.waitingForLevel)
                    Log("bot %s: no level to spawn into; it waits", bot.name);
                bot.waitingForLevel = true;
                return;
            }
            bot.waitingForLevel = false;
            bot.deadSeconds = 0.0f;
            // A new life: nothing from the last one (target, route, stuck timers), but the same leaning.
            brain::State fresh;
            fresh.rng = bot.brain.rng;
            fresh.style = bot.brain.style;
            bot.brain = fresh;
            Log("bot %s: respawn", bot.name);
            SpawnBot(context, bot);
        }
        return;
    }
    if (!bot.hadSoldier)
    {
        const float* t = nullptr;
        if (eng::GetSoldierTransform(soldier, &t) && t)
        {
            int pose = 0, state = 0;
            bool known = eng::GetCharacterPose(soldier, &pose, nullptr, &state);
            float feet[3] = {t[12], t[13], t[14]};
            float water = eng::WaterDepth(feet);
            char waterText[48] = "";
            if (water > 0.0f)
                std::snprintf(waterText, sizeof(waterText), ", in %.1f m of WATER", water);
            Log("bot %s: has soldier %p at (%.1f, %.1f, %.1f), %s, %s%s", bot.name, soldier, t[12], t[13], t[14],
                known ? eng::PoseName(pose) : "pose ?", known ? eng::StateName(state) : "state ?", waterText);
        }
        bot.hadSoldier = true;
        bot.deadSeconds = 0.0f;
    }

    // Dead on your screen only (a ragdoll there that the server still moves about): the server kills it too.
    unsigned shownMs = 0;
    float serverHealth = 0.0f;
    if (std::find(g_kicked.begin(), g_kicked.end(), bot.player) != g_kicked.end())
    {
        eng::KillSoldier(soldier);
        return;
    }
    limp::CheckCopy(bot.player, soldier, bot.name);
    if (!StandDown() && limp::ShownDeadOnlyThere(bot.player, soldier, &shownMs, &serverHealth))
    {
        if (limp::WatchOnly())
        {
            Log("bot %s: your game's estimate has had it dead for %u ms while the server has it alive (health %.0f); "
                "its body follows the server, so nothing should show",
                bot.name, shownMs, serverHealth);
        }
        else
        {
            Log("bot %s: your game has shown it dead for %u ms while the server has it alive (health %.0f) - a limp "
                "body on your screen: killed on the server too",
                bot.name, shownMs, serverHealth);
            eng::KillSoldier(soldier);
            return;
        }
    }

    // Movement, aim, fire, jump, sprint, reload: the brain writes the whole input (or a neutral one when paused).
    // In a stealth kill the animation has it, as the game's own AI brain leaves it be (0x1854F3B).
    if (brain::Enabled() && !StandDown() && !melee::Involved(soldier))
        brain::Update(bot.brain, bot.name, bot.player, soldier, bot.input, bot.team, bot.kitType, actors, dt);
    else
        brain::Idle(bot.input);

    auto* input = static_cast<char*>(bot.input);
    // The engine applies an AI player's input every frame (0xB43230) with the input's own deltaTime. Left at 0, the
    // character never moved or settled: bots stood half in the ground and couldn't be hit. This was the fix.
    *reinterpret_cast<float*>(input + eng::kInput_DeltaTime) = dt;

    // Step the soldier with that input every tick, as fun-bots does in VU ("engine-requirement"). Bot soldiers report
    // "not single-step", but nothing else runs their character physics: without this they turn but never walk.
    if (bot.stepDisabled)
        return;
    bool stepped = false;
    bool ok = eng::StepSoldier(soldier, g_config.alwaysStep != 0, &stepped);
    if (!ok)
    {
        bot.stepDisabled = true;  // a fault every tick would flood the log (and the engine)
        Log("bot %s: singleStepEntry FAILED; no longer stepping this bot", bot.name);
    }
    else if (!bot.loggedStep)
    {
        Log("bot %s: %s", bot.name,
            stepped ? "single-stepping its soldier each tick" : "soldier isn't single-step (AlwaysStep=0; not stepped)");
        bot.loggedStep = true;
    }
}

// Diagnostic: find a "MOHW/Levels/..." / "Levels/..." name reachable from the server level object (directly or via
// one pointer). The exact field isn't mapped yet; this tells us which level the server has loaded.
void LogLevelName(void* level)
{
    for (int off = 0; off < 0x200; off += 4)
    {
        void* p = nullptr;
        if (!eng::ProbePtr(At(level, off), &p) || !p)
            continue;
        for (int depth = 0; depth < 2; ++depth)
        {
            char text[48] = {};
            bool ok = true;
            for (int i = 0; i < 47 && ok; ++i)
            {
                uint8_t c = 0;
                ok = eng::ProbeByte(static_cast<char*>(p) + i, &c);
                text[i] = static_cast<char>(c);
                if (!c)
                    break;
            }
            if (ok && (std::strncmp(text, "MOHW/Levels/", 12) == 0 || std::strncmp(text, "Levels/", 7) == 0))
            {
                Log("  level name at level+%#x%s: %s", off, depth ? " (via pointer)" : "", text);
                return;
            }
            void* inner = nullptr;
            if (depth == 0 && !(eng::ProbePtr(p, &inner) && inner))
                break;
            p = inner;
        }
    }
    Log("  level name: not found near level object %p", level);
}

void Dump(void* context)
{
    Log("---- dump: ServerGameContext %p, %u bots, brain %s", context, static_cast<unsigned>(g_bots.size()),
        brain::Enabled() ? "on" : "paused");
    if (!context)
        return;
    void* manager = PlayerManager(context);
    uint32_t maxPlayers = 0;
    if (manager)
        eng::ReadU32(At(manager, eng::kMgr_MaxPlayers), &maxPlayers);
    uint32_t teamCount = 0;
    TeamDataFor(context, 0, &teamCount);
    void* level = nullptr;
    void* teams = nullptr;
    eng::ReadPtr(At(context, eng::kCtx_Level), &level);
    if (level)
        eng::ReadPtr(At(level, eng::kLevel_Teams), &teams);
    Log("  ServerPlayerManager %p, max players %u, level %p, teams %p, team count %u", manager, maxPlayers, level, teams,
        teamCount);
    if (level)
        LogLevelName(level);
    if (!manager)
        return;
    for (void* p : Players(manager))
    {
        uint32_t team = 0;
        void* input = nullptr;
        void* controlled = nullptr;
        void* asset = nullptr;
        eng::ReadU32(At(p, eng::kPlayer_Team), &team);
        eng::ReadPtr(At(p, eng::kPlayer_Input), &input);
        eng::ReadPtr(At(p, eng::kPlayer_Controlled), &controlled);
        eng::ReadPtr(At(p, eng::kPlayer_SoldierAsset), &asset);
        void* soldier = SoldierOf(p);
        const float* t = nullptr;
        if (soldier)
            eng::GetSoldierTransform(soldier, &t);
        Log("  player %p ai=%d team=%u input=%p soldier=%p controlled=%p asset=%p pos=(%.1f, %.1f, %.1f)", p, IsAI(p), team,
            input, soldier, controlled, asset, t ? t[12] : 0.0f, t ? t[13] : 0.0f, t ? t[14] : 0.0f);
        // Selected unlocks (ServerPlayer+0x524): what spawn customisation builds the soldier from.
        LogPlayerUnlocks(p, "    unlocks");
    }
}

// Online, matchmaking assigns each joining player a team and the game mode waits for it (pf_waitforteamexternal);
// offline that never happens, the player stays on team 0, which has no spawn points, and spawning stalls at
// "Falling back". Puts every human still on team 0 onto humanTeam once the level has that team.
void AssignHumanTeams(void* context)
{
    if (g_config.humanTeam <= 0 || !TeamDataFor(context, g_config.humanTeam, nullptr))
        return;
    void* manager = PlayerManager(context);
    if (!manager)
        return;
    for (void* p : Players(manager))
    {
        uint32_t team = 0;
        if (IsAI(p) || !eng::ReadU32(At(p, eng::kPlayer_Team), &team) || team != 0)
            continue;
        bool ok = eng::SetTeam(p, g_config.humanTeam);
        static int logged = 0;  // runs every tick; if the game keeps resetting the team, say so a few times only
        if (logged < 5)
            Log("human player %p had no team -> team %d%s%s", p, g_config.humanTeam, ok ? "" : " (setTeam failed)",
                ++logged == 5 ? " (further reassignments not logged)" : "");
    }
}

// Tops both teams up to TeamSize with bots, one bot every 0.4 s, starting 3 s after you have a team (so you are
// counted on yours): 10 = you + 9 bots against 10. Players already there count, humans and bots alike; a bot waiting
// to come back as a new player counts for its team. Stops while the round is over.
void FillTeams(void* context, float dt)
{
    static float sinceTeam = 0.0f, sinceAdd = 0.0f;
    static bool announced = false;
    if (g_config.teamSize <= 0 || StandDown() || g_spawnsHalted)
        return;
    void* manager = PlayerManager(context);
    std::vector<void*> players = manager ? Players(manager) : std::vector<void*>{};
    void* host = FindHost(players);
    uint32_t hostTeam = 0;
    if (!host || !eng::ReadU32(At(host, eng::kPlayer_Team), &hostTeam) || (hostTeam != 1 && hostTeam != 2) ||
        !TeamDataFor(context, static_cast<int>(hostTeam), nullptr))
    {
        sinceTeam = 0.0f;
        announced = false;
        return;
    }
    sinceTeam += dt;
    sinceAdd += dt;
    if (sinceTeam < 3.0f || sinceAdd < 0.4f)
        return;
    int count[3] = {};
    for (void* p : players)
    {
        uint32_t team = 0;
        if (eng::ReadU32(At(p, eng::kPlayer_Team), &team) && team < 3)
            ++count[team];
    }
    for (const PendingRespawn& r : g_respawns)
        if (r.team > 0 && r.team < 3)
            ++count[r.team];
    int ours = static_cast<int>(hostTeam);
    int theirs = ours == 1 ? 2 : 1;
    int needOurs = g_config.teamSize - count[ours];
    int needTheirs = g_config.teamSize - count[theirs];
    if (needOurs <= 0 && needTheirs <= 0)
    {
        if (!announced)
            Log("teams: filled - team %d %d (you + %d bots), team %d %d", ours, count[ours], count[ours] - 1, theirs,
                count[theirs]);
        announced = true;
        return;
    }
    announced = false;
    sinceAdd = 0.0f;
    // The team further from full first, so both grow together.
    bool friendly = needOurs >= needTheirs;
    int before = static_cast<int>(g_bots.size());
    AddBot(context, false, friendly, nullptr, true);
    if (static_cast<int>(g_bots.size()) == before)
        sinceAdd = -5.0f;  // it failed (logged): try again in a few seconds
}

// Once a minute: how much server-thread time the DLL takes per tick (bots, brain, rays) and the server tick rate,
// so slowdowns can be told apart from the game's own.
void LogPerformance(const LARGE_INTEGER& start, float dt)
{
    static LARGE_INTEGER frequency = {};
    static double totalUs = 0.0, maxUs = 0.0, totalDt = 0.0;
    static uint32_t ticks = 0;
    static DWORD windowStart = GetTickCount();
    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double us = static_cast<double>(now.QuadPart - start.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);
    totalUs += us;
    maxUs = us > maxUs ? us : maxUs;
    totalDt += dt;
    ++ticks;
    DWORD elapsed = GetTickCount() - windowStart;
    if (elapsed < 60000)
        return;
    uint32_t rays = eng::TakeRayCount();
    Log("perf: %u server ticks in %.0f s (avg dt %.1f ms), bot work avg %.0f us / max %.0f us per tick, %.0f rays/s, "
        "%u bots",
        ticks, elapsed / 1000.0, totalDt * 1000.0 / ticks, totalUs / ticks, maxUs, rays * 1000.0 / elapsed,
        static_cast<unsigned>(g_bots.size()));
    totalUs = maxUs = totalDt = 0.0;
    ticks = 0;
    windowStart = GetTickCount();
}

// "MP_03_TeamDeathMatch": the server level's name (char* at level+0x68, "MOHW/Levels/MP/MP_03/MP_03") without the
// path, plus the GameMode value of GameSettings.DefaultLayerInclusion (layers add and remove props per mode).
std::string LevelKey(void* context)
{
    void* level = nullptr;
    void* namePtr = nullptr;
    char name[128] = {};
    if (!eng::ReadPtr(At(context, eng::kCtx_Level), &level) || !level ||
        !eng::ProbePtr(At(level, eng::kLevel_Name), &namePtr) || !namePtr)
        return {};
    for (size_t i = 0; i + 1 < sizeof(name); ++i)
    {
        uint8_t c = 0;
        if (!eng::ProbeByte(static_cast<char*>(namePtr) + i, &c) || !c)
            break;
        name[i] = static_cast<char>(c);
    }
    if (std::strncmp(name, "MOHW/Levels/", 12) != 0 && std::strncmp(name, "Levels/", 7) != 0)
        return {};
    const char* base = std::strrchr(name, '/');
    std::string key = base ? base + 1 : name;

    void* settings = nullptr;
    void* inclusion = nullptr;
    char text[128] = {};
    if (eng::GetSettingsContainer(eng::kTypeInfo_GameSettings, &settings) && settings &&
        eng::ProbePtr(At(settings, eng::kGameSettings_DefaultLayerInclusion), &inclusion) && inclusion)
        for (size_t i = 0; i + 1 < sizeof(text); ++i)
        {
            uint8_t c = 0;
            if (!eng::ProbeByte(static_cast<char*>(inclusion) + i, &c) || !c)
                break;
            text[i] = static_cast<char>(c);
        }
    if (const char* mode = std::strstr(text, "GameMode="))
    {
        key += '_';
        for (const char* p = mode + 9; *p && *p != ';'; ++p)
            key += *p;
    }
    for (char& c : key)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            c = '_';
    return key;
}

// Loads the navigation graph for the server's current level (and saves the previous one) when the level changes.
// Checked when the level object changes and every 5 s (a new level can reuse the old one's address).
void UpdateNavLevel(void* context, float dt)
{
    static void* lastLevel = nullptr;
    static float sinceCheck = 0.0f;
    void* level = nullptr;
    eng::ReadPtr(At(context, eng::kCtx_Level), &level);
    if (levelchange::LeavingLevel(level))
        return;  // the old level, read with the new mode: neither level's key
    sinceCheck += dt;
    if (level == lastLevel && sinceCheck < 5.0f)
        return;
    lastLevel = level;
    sinceCheck = 0.0f;
    const std::string key = level ? LevelKey(context) : std::string();
    if (!key.empty() && !g_levelKey.empty() && key != g_levelKey)
    {
        // A new level in the same game (in-place change): routes, targets and goals from the last one mean nothing here.
        for (Bot& bot : g_bots)
        {
            brain::State fresh;
            fresh.rng = bot.brain.rng;
            fresh.style = bot.brain.style;
            bot.brain = fresh;
            bot.deadSeconds = 0.0f;
        }
        if (g_bots.empty())
            Log("level: %s -> %s", g_levelKey.c_str(), key.c_str());
        else
            Log("level: %s -> %s; %u bots carried over (fresh brains)", g_levelKey.c_str(), key.c_str(),
                static_cast<unsigned>(g_bots.size()));
    }
    if (!key.empty())
        g_levelKey = key;
    nav::OnLevel(key);
    objectives::OnLevel(key);
}

// When each living soldier was first seen (tick count), for spawn ages.
std::unordered_map<void*, DWORD> g_soldierSeen;

// Each soldier's last position and smoothed horizontal speed, for footstep hearing and noticing.
struct Motion
{
    float pos[3] = {};
    DWORD tick = 0;
    float speed = 0.0f;
};
std::unordered_map<void*, Motion> g_motion;

// Every living soldier this tick (position, team, bot or human, seconds since it spawned, pose, movement state,
// speed, when it last fired) for the brain's perception and goals, and the spawn rules.
std::vector<brain::Actor> Actors(void* context)
{
    std::vector<brain::Actor> actors;
    void* manager = PlayerManager(context);
    if (!manager)
        return actors;
    DWORD now = GetTickCount();
    for (void* p : Players(manager))
    {
        void* soldier = SoldierOf(p);
        const float* t = nullptr;
        uint32_t team = 0;
        if (!soldier || !eng::GetSoldierTransform(soldier, &t) || !t || !eng::ReadU32(At(p, eng::kPlayer_Team), &team))
            continue;
        brain::Actor a;
        a.player = p;
        a.soldier = soldier;
        a.team = static_cast<int>(team);
        a.pos[0] = t[12];
        a.pos[1] = t[13];
        a.pos[2] = t[14];
        a.isBot = IsAI(p);
        auto seen = g_soldierSeen.emplace(soldier, now).first;
        a.spawnAge = (now - seen->second) / 1000.0f;
        eng::GetCharacterPose(soldier, &a.pose, nullptr, &a.state);
        auto motion = g_motion.find(soldier);
        if (motion == g_motion.end())
        {
            Motion m;
            std::memcpy(m.pos, a.pos, sizeof(m.pos));
            m.tick = now;
            g_motion[soldier] = m;
        }
        else if (now - motion->second.tick >= 100)  // measured over >= 0.1 s, smoothed
        {
            Motion& m = motion->second;
            float dx = a.pos[0] - m.pos[0], dz = a.pos[2] - m.pos[2];
            float speed = std::sqrt(dx * dx + dz * dz) / ((now - m.tick) / 1000.0f);
            m.speed = speed > 20.0f ? m.speed : m.speed * 0.5f + speed * 0.5f;  // ignore teleports
            std::memcpy(m.pos, a.pos, sizeof(m.pos));
            m.tick = now;
        }
        a.speed = g_motion[soldier].speed;
        a.sinceFired = spotting::SinceFired(p);
        actors.push_back(a);
    }
    if (g_soldierSeen.size() > actors.size() + 64)  // forget soldiers that are gone
        for (auto it = g_soldierSeen.begin(); it != g_soldierSeen.end();)
        {
            bool alive = false;
            for (const brain::Actor& a : actors)
                alive |= a.soldier == it->first;
            it = alive ? std::next(it) : g_soldierSeen.erase(it);
        }
    if (g_motion.size() > actors.size() + 64)
        for (auto it = g_motion.begin(); it != g_motion.end();)
        {
            bool alive = false;
            for (const brain::Actor& a : actors)
                alive |= a.soldier == it->first;
            it = alive ? std::next(it) : g_motion.erase(it);
        }
    return actors;
}

// Your spawns and the kills you're part of, to check the spawn rules and spawn kills: where you spawned relative to
// your fireteam buddy and the enemies, and who killed you how soon after spawning.
void LogHumanSpawn(const brain::Actor& you, const std::vector<brain::Actor>& actors)
{
    char name[24];
    void* buddy = nullptr;
    eng::GetFireteamBuddy(you.player, &buddy);
    float buddyDist = -1.0f;
    float nearest = 1e9f;
    const brain::Actor* nearestEnemy = nullptr;
    int inSight = 0;
    float chest[3] = {you.pos[0], you.pos[1] + 1.1f, you.pos[2]};
    for (const brain::Actor& a : actors)
    {
        float d = Distance(you.pos, a.pos);
        if (a.player == buddy)
            buddyDist = d;
        if (a.team == you.team || a.team == 0)
            continue;
        if (d < nearest)
        {
            nearest = d;
            nearestEnemy = &a;
        }
        float eye[3] = {a.pos[0], a.pos[1] + 1.6f, a.pos[2]};
        bool blocked = true;
        if (d < 250.0f && eng::RayBlocked(eye, chest, kSightFlags, &blocked) && !blocked)
            ++inSight;
    }
    char buddyText[64] = "no fireteam buddy";
    if (buddy)
        std::snprintf(buddyText, sizeof(buddyText), buddyDist >= 0.0f ? "buddy %s %.0f m away" : "buddy %s (dead)",
                      NameOf(buddy, name, sizeof(name)), buddyDist);
    char enemyName[24];
    Log("you spawned at (%.1f, %.1f, %.1f): %s; nearest enemy %s %.0f m; %d enemies with a line of sight to you",
        you.pos[0], you.pos[1], you.pos[2], buddyText,
        nearestEnemy ? NameOf(nearestEnemy->player, enemyName, sizeof(enemyName)) : "-", nearestEnemy ? nearest : 0.0f,
        inSight);
}

std::vector<brain::Actor> g_lastActors;  // the previous tick's, for kills (the victim is gone by then)

const brain::Actor* LastActor(void* player)
{
    for (const brain::Actor& a : g_lastActors)
        if (a.player == player)
            return &a;
    return nullptr;
}

// MOHW fireteams are pairs: a player's squad id (VeniceServerPlayerExtent +0xF4, 5 = none) decides who their buddy
// is (getFireteamBuddy: the teammate with the same id), for buddy spawns and the buddy score bonus. Online the
// server's squad handling assigns it; offline every player kept 5, so "the buddy" was just the first teammate in the
// player list - a different bot after every death. This pairs players up with the game's own setSquad: you with a
// bot on your team, the other bots with each other (5 fireteams per team); a respawned bot rejoins its old fireteam.
std::unordered_map<std::string, int> g_fireteamOf;  // bot name -> squad it was put in
int g_humanFireteam = -1;
std::unordered_map<void*, int> g_squadAttempts;  // player -> times setSquad was tried (a few, if it doesn't stick)

void AssignFireteams(void* context)
{
    if (!g_config.fireteams)
        return;
    void* manager = PlayerManager(context);
    if (!manager)
        return;
    struct Member
    {
        void* player;
        int team;
        int squad;
        bool ai;
    };
    std::vector<Member> members;
    int count[3][eng::kNoSquad] = {};  // [team 1..2][squad 0..4]
    for (void* p : Players(manager))
    {
        uint32_t team = 0;
        int squad = eng::kNoSquad;
        if (!eng::ReadU32(At(p, eng::kPlayer_Team), &team) || team < 1 || team > 2 || !eng::GetSquad(p, &squad))
            continue;
        members.push_back({p, static_cast<int>(team), squad, IsAI(p)});
        if (squad >= 0 && squad < eng::kNoSquad)
            ++count[team][squad];
    }
    // You first, so you get a bot buddy before the bots pair up among themselves.
    std::stable_sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return !a.ai && b.ai; });
    for (Member& m : members)
    {
        if (m.squad >= 0 && m.squad < eng::kNoSquad)
            continue;
        const Bot* bot = m.ai ? FindBot(m.player) : nullptr;
        if ((m.ai && !bot) || g_squadAttempts[m.player] >= 3)
            continue;  // not one of ours, or the game keeps resetting it
        int remembered = m.ai ? (g_fireteamOf.count(bot->name) ? g_fireteamOf[bot->name] : -1) : g_humanFireteam;
        int want = -1;
        if (remembered >= 0 && count[m.team][remembered] < 2)
            want = remembered;
        for (int k = 0; want < 0 && k < eng::kNoSquad; ++k)
            if (count[m.team][k] == 1)
                want = k;  // someone waiting for a buddy
        for (int k = 0; want < 0 && k < eng::kNoSquad; ++k)
            if (count[m.team][k] == 0)
                want = k;
        if (want < 0)
            continue;  // 10 players on the team already paired
        ++g_squadAttempts[m.player];
        bool ok = eng::SetSquad(m.player, want);
        int now = eng::kNoSquad;
        eng::GetSquad(m.player, &now);
        ++count[m.team][want];
        if (m.ai)
            g_fireteamOf[bot->name] = want;
        else
            g_humanFireteam = want;
        static int logged = 0;
        if (logged < 40 || !ok || now != want)
        {
            ++logged;
            Log("fireteams: %s (team %d) -> fireteam %d%s", m.ai ? bot->name : "you", m.team, want,
                !ok ? " (setSquad FAILED)" : now != want ? " (didn't stick)" : "");
        }
    }
}

// For the combat log: a bot's name / kit type, or null / 0 for players that aren't ours.
const char* BotNameOf(void* player)
{
    auto it = g_botHistory.find(player);
    return it != g_botHistory.end() ? it->second.first.c_str() : nullptr;
}

int BotKitOf(void* player)
{
    auto it = g_botHistory.find(player);
    return it != g_botHistory.end() ? it->second.second : 0;
}

// For the scoreboard log: a bot's name, "you" for a human.
const char* RowNameOf(void* player)
{
    if (const char* name = BotNameOf(player))
        return name;
    return IsAI(player) ? "(another AI player)" : "you";
}

// For the combat log's bullet tracing: a bot's aim as written to its input last tick, and its target.
bool BotAimOf(void* player, float* yaw, float* pitch, void** target)
{
    for (const Bot& b : g_bots)
        if (b.player == player)
        {
            *yaw = b.brain.yaw;
            *pitch = b.brain.pitch;
            *target = b.brain.target;
            return true;
        }
    return false;
}

// Scores kills while the online stats object is missing (scoring.cpp) and logs the kills you're part of.
void ProcessKills(void* context, const std::vector<scoring::Kill>& kills)
{
    if (kills.empty())
        return;
    std::vector<void*> players = Players(PlayerManager(context));
    for (const scoring::Kill& kill : kills)
    {
        bool killerPresent = std::find(players.begin(), players.end(), kill.killer) != players.end();
        bool forHumanBuddy = false;
        if (killerPresent && kill.killerIsAI)
        {
            void* buddy = nullptr;
            uint8_t ai = 1;
            forHumanBuddy = eng::GetFireteamBuddy(kill.killer, &buddy) && buddy &&
                            eng::ReadByte(At(buddy, eng::kPlayer_IsAI), &ai) && !ai;
        }
        if (killerPresent)
            scoring::Award(kill, forHumanBuddy);
        if (killerPresent && kill.killer != kill.victim && kill.killerTeam != kill.victimTeam)
            scoreboard::CreditKill(kill.killer, kill.headshot);
        NoteDeath(kill, LastActor(kill.victim));
        if (kill.killer && kill.killer != kill.victim && kill.killerTeam == kill.victimTeam)
        {
            char teamA[24], teamB[24];
            Log("team kill: %s -> %s with %s", NameOf(kill.killer, teamA, sizeof(teamA)), NameOf(kill.victim, teamB, sizeof(teamB)),
                kill.weapon[0] ? kill.weapon : "?");
        }
        if (kill.victimIsAI && (!kill.killer || kill.killerIsAI))
            continue;
        const brain::Actor* victim = LastActor(kill.victim);
        const brain::Actor* killer = kill.killer ? LastActor(kill.killer) : nullptr;
        char a[24], b[24];
        const Bot* bot = killerPresent ? FindBot(kill.killer) : nullptr;
        Log("kill: %s%s -> %s%s, %.0f m; the victim had been alive %.1f s",
            kill.killer ? NameOf(kill.killer, a, sizeof(a)) : "(world)",
            bot && bot->kitType == 4 ? " (sniper)" : "", kill.victimIsAI ? NameOf(kill.victim, b, sizeof(b)) : "you",
            kill.headshot ? " (headshot)" : "", victim && killer ? Distance(victim->pos, killer->pos) : -1.0f,
            victim ? victim->spawnAge : -1.0f);
    }
}

// Scores what the objective modes' tokens hand out (a charge armed or disarmed, an objective destroyed, a Home Run
// flag; scoring.cpp). Yours get the game's score message - the "+200" popup, the buddy bonus, the support-action
// meter and the armed / disarmed count on the end-of-round screen; every row (bots' too) gets the points.
void ProcessTokenEvents(void* context, const std::vector<scoring::TokenEvent>& events)
{
    if (events.empty())
        return;
    std::vector<void*> players = Players(PlayerManager(context));
    struct Share
    {
        int statEvent, bots;
    };
    std::vector<Share> shares;  // per event: how many bots got it
    for (const scoring::TokenEvent& e : events)
    {
        if (std::find(players.begin(), players.end(), e.player) == players.end())
            continue;
        int points = 0;
        const char* what = nullptr;
        if (!scoring::TokenScore(e.statEvent, &points, &what))
        {
            static int unknown = 0;
            if (unknown++ < 10)
                Log("scoring: token stat event %d (player %p) has no score entry; not scored", e.statEvent, e.player);
            continue;
        }
        if (IsAI(e.player))
        {
            scoreboard::CreditPoints(e.player, points);
            auto share = std::find_if(shares.begin(), shares.end(), [&](const Share& s) { return s.statEvent == e.statEvent; });
            if (share == shares.end())
                shares.push_back({e.statEvent, 1});
            else
                ++share->bots;
            continue;
        }
        bool sent = scoring::AwardToken(e);
        if (sent)
            scoreboard::CreditPoints(e.player, points);
        int count = 0, total = 0;
        char name[24], counts[96] = "";
        if (scoreboard::ReadObjectives(e.player, &count, &total))  // the total follows within 0.25 s (scoreboard::Tick)
            std::snprintf(counts, sizeof(counts), "; objectives on the row %d", count);
        Log("scoring: %s: %s, +%d %s%s", NameOf(e.player, name, sizeof(name)), what, points,
            sent ? "(score event sent)" : "(not scored: [scoring] Enabled=0, or the online stats object exists)", counts);
    }
    for (const Share& s : shares)
    {
        int points = 0;
        const char* what = nullptr;
        scoring::TokenScore(s.statEvent, &points, &what);
        Log("scoring: %s, +%d on the rows of %d bot(s)", what, points, s.bots);
    }
}

// Logs level / team / player changes as they happen, so a load can be followed without pressing keys.
void LogStatusChanges(void* context)
{
    static void* lastLevel = nullptr;
    static uint32_t lastTeams = 0;
    static size_t lastPlayers = 0;
    void* level = nullptr;
    uint32_t teamCount = 0;
    size_t players = 0;
    if (context)
    {
        eng::ReadPtr(At(context, eng::kCtx_Level), &level);
        TeamDataFor(context, 0, &teamCount);
        if (void* manager = PlayerManager(context))
            players = Players(manager).size();
    }
    if (level == lastLevel && teamCount == lastTeams && players == lastPlayers)
        return;
    Log("status: level %p, team count %u, players %u", level, teamCount, static_cast<unsigned>(players));
    if (level && level != lastLevel)
        LogLevelName(level);
    lastLevel = level;
    lastTeams = teamCount;
    lastPlayers = players;
}

// Once a minute: how bunched up the bots were (teammates within 8 m of a living bot, on average over the minute) and
// where they headed when they had no target - to see whether teams still move as one pack.
void LogSpread(const std::vector<brain::Actor>& actors, float dt)
{
    static double nearSum = 0.0, samples = 0.0;
    static int packed = 0, ticks = 0;
    static float timer = 0.0f;
    int bots = 0, bunched = 0;
    for (const brain::Actor& a : actors)
    {
        if (!a.isBot)
            continue;
        int close = 0;
        for (const brain::Actor& b : actors)
            if (&b != &a && b.isBot && b.team == a.team && Distance(a.pos, b.pos) < 8.0f)
                ++close;
        nearSum += close;
        samples += 1.0;
        ++bots;
        bunched += close >= 2;
    }
    packed += bots && bunched * 2 > bots;  // most bots had two or more teammates within 8 m
    ++ticks;
    timer += dt;
    if (timer < 60.0f)
        return;
    int goals[brain::kGoalKinds];
    brain::TakeGoalCounts(goals);
    Log("spread (last minute): a bot had %.1f teammates within 8 m on average; most bots in packs of 3+ %.0f%% of the "
        "time; goals with no target: hunt %d, flank %d, roam %d, objective %d", samples > 0.0 ? nearSum / samples : 0.0,
        ticks ? 100.0 * packed / ticks : 0.0, goals[brain::kGoalHunt], goals[brain::kGoalFlank],
        goals[brain::kGoalRoam], goals[brain::kGoalObjective]);
    nearSum = samples = 0.0;
    packed = ticks = 0;
    timer = 0.0f;
}

}  // namespace

void Init()
{
    g_config.keySpawnHere = ReadIni(L"KeyAddBot", g_config.keySpawnHere);
    g_config.keySpawnAtMe = ReadIni(L"KeyAddBotAtMe", g_config.keySpawnAtMe);
    g_config.keyToggleMove = ReadIni(L"KeyToggleMove", g_config.keyToggleMove);
    g_config.keyDump = ReadIni(L"KeyDump", g_config.keyDump);
    g_config.keySpawnFriendly = ReadIni(L"KeyAddFriendlyBot", g_config.keySpawnFriendly);
    g_config.team = ReadIni(L"Team", g_config.team);
    g_config.humanTeam = ReadIni(L"HumanTeam", g_config.humanTeam);
    g_config.kitIndex = ReadIni(L"KitIndex", g_config.kitIndex);
    g_config.blueprint = ReadIni(L"Blueprint", g_config.blueprint);
    g_config.unlocks = ReadIni(L"Unlocks", g_config.unlocks);
    g_config.autoRespawnSeconds = ReadIni(L"AutoRespawnSeconds", g_config.autoRespawnSeconds);
    g_config.alwaysStep = ReadIni(L"AlwaysStep", g_config.alwaysStep);
    g_config.fireteams = ReadIni(L"Fireteams", g_config.fireteams);
    g_config.spawnOnBuddy = ReadIni(L"SpawnOnBuddy", g_config.spawnOnBuddy);
    g_config.buddySafeMeters = ReadIni(L"BuddySafeMeters", g_config.buddySafeMeters);
    g_config.fallBackToOwnBase = ReadIni(L"FallBackToOwnBase", g_config.fallBackToOwnBase);
    g_config.sharedSpawnWhenCamped = ReadIni(L"SharedSpawnWhenCamped", g_config.sharedSpawnWhenCamped);
    g_config.campedEnemies = ReadIni(L"CampedEnemies", g_config.campedEnemies);
    g_config.campedSpawnDeaths = ReadIni(L"CampedSpawnDeaths", g_config.campedSpawnDeaths);
    g_config.campedSeconds = ReadIni(L"CampedSeconds", g_config.campedSeconds);
    g_config.sharedSpawnSafeMeters = ReadIni(L"SharedSpawnSafeMeters", g_config.sharedSpawnSafeMeters);
    g_config.demoMiniGrenades = ReadIni(L"DemoMiniGrenades", g_config.demoMiniGrenades);
    g_config.keepPlayers = ReadIni(L"KeepPlayers", g_config.keepPlayers);
    g_config.teamSize = ReadIni(L"TeamSize", g_config.teamSize);
    ReadNames();
    Log("config: add=0x%02X addAtMe=0x%02X addFriendly=0x%02X brain=0x%02X dump=0x%02X team=%d humanTeam=%d kit=%d "
        "blueprint=%d unlocks=%d respawn=%ds",
        g_config.keySpawnHere, g_config.keySpawnAtMe, g_config.keySpawnFriendly, g_config.keyToggleMove,
        g_config.keyDump, g_config.team, g_config.humanTeam, g_config.kitIndex, g_config.blueprint,
        g_config.unlocks, g_config.autoRespawnSeconds);
    Log("names: %u (%s%s)", static_cast<unsigned>(g_names.size()), g_names.empty() ? "" : g_names.front().c_str(),
        g_names.size() > 1 ? ", ..." : "");
    if (g_config.teamSize > 0)
        Log("teams: filled with bots to %d a side once you have a team (%d bots on yours, %d against you)",
            g_config.teamSize, g_config.teamSize - 1, g_config.teamSize);
    InstallOwnBaseSpawnHooks();
    objectives::Install();
    InstallKeepPlayers();
    brain::Init();
    nav::Init();
    scoring::Init();
    scoreboard::Init();
    spotting::Init();
    combatlog::Init();
    limp::Init();
    melee::Init();
    hardcore::Init();
    rcon::Init();
}

void OnServerUpdate(void* server, void* updateInfo)
{
    static bool logged = false;
    if (!logged)
    {
        Log("first Server::update tick (server %p)", server);
        logged = true;
    }

    ApplyOverrides();

    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(static_cast<char*>(server) + eng::kServer_UpdateLock);
    if (!TryEnterCriticalSection(lock))
        return;

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    float dt = 0.0f;
    eng::ReadFloat(At(updateInfo, 4), &dt);
    if (!(dt > 0.0f && dt < 0.5f))
        dt = 1.0f / 30.0f;

    void* context = eng::ServerGameContext();
    if (context != g_lastContext)
    {
        Log("ServerGameContext changed: %p -> %p", g_lastContext, context);
        // What decides whether bot soldiers' server-side hit shapes move: the server's game type (setup = server
        // +0x60: +0x73 clear = singleplayer, +0x74 = coop, else multiplayer) and its animation manager (context +0x18,
        // created at startup only for multiplayer unless ForceServerAnimation).
        void* setup = nullptr;
        uint8_t mp = 0, coop = 0;
        void* animation = nullptr;
        bool known = eng::ReadPtr(At(server, 0x60), &setup) && setup && eng::ReadByte(At(setup, 0x73), &mp) &&
                     eng::ReadByte(At(setup, 0x74), &coop);
        if (context)
            eng::ReadPtr(At(context, 0x18), &animation);
        Log("server: game type %s; server animation manager %p (%s)",
            !known ? "?" : !mp ? "singleplayer" : coop ? "coop" : "multiplayer", animation,
            animation ? "soldiers' server-side hit shapes are updated" : "NONE: bot soldiers' hit shapes never move");
        // The other two conditions for creating it (0xAA9FBE: setup +0xA8 array count; GameAnimationSettings
        // .ServerEnable +0x44).
        void* list = nullptr;
        uint32_t listCount = 0xFFFFFFFF;
        if (setup && eng::ReadPtr(At(setup, 0xA8), &list) && list)
            eng::ReadU32(At(list, -4), &listCount);
        void* anim = nullptr;
        uint8_t serverEnable = 0xFF, clientEnable = 0xFF;
        if (eng::GetSettingsContainer(0x2B74558, &anim) && anim)
        {
            eng::ReadByte(At(anim, 0x44), &serverEnable);
            eng::ReadByte(At(anim, 0x46), &clientEnable);
        }
        Log("server: setup +0xA8 list %p, %d entries (none = no server animation); GameAnimationSettings.ServerEnable "
            "%d, ClientEnable %d", list, static_cast<int>(listCount), serverEnable, clientEnable);
        g_lastContext = context;
        g_respawns.clear();
        g_botHistory.clear();
        g_fireteamOf.clear();
        g_humanFireteam = -1;
        g_squadAttempts.clear();
        g_kicked.clear();
        for (BaseWatch& base : g_bases)
            base = BaseWatch();
        g_pointPositionMisses = 0;
        scoreboard::Reset();
        InterlockedExchange(&g_roundOver, 0);
    }
    LogStatusChanges(context);
    ValidateBots(context);

    if (context)
    {
        AssignHumanTeams(context);
        AssignFireteams(context);
        FillTeams(context, dt);
        if (KeyPressed(g_config.keySpawnHere))
            AddBot(context, false, false);
        if (KeyPressed(g_config.keySpawnAtMe))
            AddBot(context, true, false);
        if (KeyPressed(g_config.keySpawnFriendly))
            AddBot(context, false, true);
        if (KeyPressed(g_config.keyToggleMove))
        {
            brain::SetEnabled(!brain::Enabled());
            Log("bot brain %s (%u bots)", brain::Enabled() ? "resumed" : "paused", static_cast<unsigned>(g_bots.size()));
            Feedback(brain::Enabled());
        }
        if (KeyPressed(g_config.keyDump))
        {
            Dump(context);
            Feedback(true);
        }
        UpdateNavLevel(context, dt);
        levelchange::ServerTick(server, context, g_levelKey, g_bots.size());
        std::vector<scoring::Kill> kills = scoring::TakeKills();
        ProcessKills(context, kills);
        ProcessTokenEvents(context, scoring::TakeTokenEvents());
        rcon::Tick(context, Players(PlayerManager(context)), &BotNameOf, kills, g_roundOver != 0, dt);
        std::vector<brain::Actor> actors = Actors(context);
        UpdateBases(actors, dt);
        objectives::Tick(actors, dt);
        combatlog::Tick(actors, kills, &BotNameOf, &BotKitOf, &BotAimOf);
        static std::unordered_map<void*, void*> humanSoldiers;  // player -> soldier already reported
        for (const brain::Actor& a : actors)
            if (!a.isBot && humanSoldiers[a.player] != a.soldier)
            {
                humanSoldiers[a.player] = a.soldier;
                LogHumanSpawn(a, actors);
            }
        // Walkers teach the bots the routes - on foot only: a swim or a slide down a slope isn't a route (a bot that
        // walked into the sea taught the graph nodes in the water). The chain restarts once back on foot.
        for (const brain::Actor& a : actors)
            if (!a.isBot || nav::LearnFromBots())
            {
                if (a.state == eng::kStateSwimming || a.state == eng::kStateSliding ||
                    a.state == eng::kStateSprintSliding)
                    nav::ForgetWalker(a.player);
                else
                    nav::Learn(a.player, a.pos, dt);
            }
        // And nodes learned in water before (earlier sessions) are found and avoided: a few per tick.
        static int wetNodes = 0, wetLogged = 0;
        static float wetLogTimer = 0.0f;
        wetNodes += nav::ScanForWater(&eng::WaterDepth, 2, 0.4f);  // 4 rays a tick; a full pass in ~10 s
        wetLogTimer += dt;
        if (wetNodes != wetLogged && wetLogTimer > 30.0f)
        {
            Log("nav: %d node(s) found with water over them; routes avoid them", wetNodes);
            wetLogged = wetNodes;
            wetLogTimer = 0.0f;
        }
        nav::Tick(dt);
        grenades::ServerTick();
        for (Bot& bot : g_bots)
            Drive(bot, context, actors, dt);
        g_lastActors = actors;
        LogSpread(actors, dt);
        std::vector<void*> everyone = Players(PlayerManager(context));
        scoreboard::Tick(everyone, dt);
        scoreboard::LogRows(everyone, &RowNameOf, dt);

        // Minimap: enemy bots that fired, and enemies your bots can see right now.
        void* human = nullptr;
        int humanTeam = 0;
        for (const brain::Actor& a : actors)
            if (!a.isBot)
            {
                human = a.player;
                humanTeam = a.team;
            }
        if (!human)  // dead: still the spotter for your team
            if (void* host = FindHost(Players(PlayerManager(context))))
            {
                uint32_t team = 0;
                eng::ReadU32(At(host, eng::kPlayer_Team), &team);
                human = host;
                humanTeam = static_cast<int>(team);
            }
        std::vector<spotting::Sighting> sightings;
        for (const Bot& bot : g_bots)
            if (bot.team == humanTeam && bot.brain.target && bot.brain.sinceSeen < 0.3f)
                sightings.push_back({bot.player, bot.brain.target});
        spotting::Tick(human, humanTeam, actors, sightings, dt);

        for (size_t i = 0; i < g_respawns.size() && !StandDown() && !g_spawnsHalted;)
        {
            g_respawns[i].waited += dt;
            if (SitsOutRound(g_respawns[i].round))
                g_respawns[i].waited = 0.0f;
            if (g_respawns[i].waited < static_cast<float>(g_config.autoRespawnSeconds))
            {
                ++i;
                continue;
            }
            PendingRespawn respawn = g_respawns[i];
            g_respawns.erase(g_respawns.begin() + i);
            AddBot(context, false, false, &respawn);
        }
        if (levelchange::BotsLeaving())
            ReleaseForLevelChange(context);
    }

    LeaveCriticalSection(lock);
    LogPerformance(start, dt);
}

void OnMessage(const void* message)
{
    const uint32_t* header = static_cast<const uint32_t*>(message);
    if (header[1] != kCategoryRound)
        return;
    if (header[2] == kRoundReset || header[2] == kSubRoundOver || header[2] == kHalfTime)
    {
        g_lastBoundaryTick = static_cast<LONG>(GetTickCount());
        LONG n = InterlockedIncrement(&g_roundBoundaries);
        if (header[2] != kRoundReset)
            Log("round: %s (round change %ld)%s", header[2] == kSubRoundOver ? "sub-round over" : "half time", n,
                modes::OneLifePerRound() ? ": bots that died get their next life" : "");
    }
    if (header[2] != kRoundOver && header[2] != kRoundReset)
        return;
    bool over = header[2] == kRoundOver;
    if (InterlockedExchange(&g_roundOver, over ? 1 : 0) != (over ? 1 : 0))
        Log(over ? "round over: bots stand down (no moving, firing or respawning) until a new round"
                 : "new round: bots back in play");
}

bool Kick(void* player)
{
    if (!FindBot(player))
        return false;
    if (std::find(g_kicked.begin(), g_kicked.end(), player) == g_kicked.end())
        g_kicked.push_back(player);
    return true;
}

}  // namespace bots
