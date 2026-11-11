#include "rcon.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "bots.h"
#include "engine.h"
#include "hardcore.h"
#include "log.h"
#include "modes.h"
#include "nav.h"
#include "levelchange.h"
#include "rconsrv.h"
#include "relaunch.h"
#include "scoreboard.h"

namespace rcon {
namespace {

// ServerPlayerChatMessage (built by 0xB05F60): +0x28 the player, +0x2C the text (wide).
constexpr uint32_t kCategoryServerPlayer = 0xF3C93DD3;
constexpr uint32_t kServerPlayerChat = 0xDBC6F766;
constexpr int kChat_Player = 0x28;
constexpr int kChat_Text = 0x2C;
constexpr int kPlayer_InlineName = 0x28;  // Player: its online name
constexpr float kTdmBaseKills = 75.0f;  // Team Deathmatch at GameModeCounterModifier 1

enum class VarKind
{
    Flag,         // vars value = the setting
    InverseFlag,  // vars value = !setting (the settings say "No...", ProCon's say what's on)
    Percent,      // vars value = setting x 100
};

struct SettingVar
{
    const char* name;
    uintptr_t typeInfo;
    int offset;
    VarKind kind;
};

const SettingVar kSettingVars[] = {
    {"vars.friendlyFire", eng::kTypeInfo_SyncedGameSettings, 0x1D, VarKind::Flag},
    {"vars.regenerateHealth", eng::kTypeInfo_SyncedGameSettings, 0x1C, VarKind::InverseFlag},
    {"vars.bulletDamage", eng::kTypeInfo_SyncedGameSettings, 0x14, VarKind::Percent},
    {"vars.playerManDownTime", eng::kTypeInfo_SyncedGameSettings, 0x10, VarKind::Percent},
    {"vars.gameModeCounter", eng::kTypeInfo_SyncedBFSettings, 0x0C, VarKind::Percent},
    {"vars.hud", eng::kTypeInfo_SyncedBFSettings, 0x11, VarKind::InverseFlag},
    {"vars.nameTag", eng::kTypeInfo_SyncedBFSettings, 0x14, VarKind::InverseFlag},
    {"vars.onlySquadLeaderSpawn", eng::kTypeInfo_SyncedBFSettings, 0x15, VarKind::Flag},
    {"vars.soldierHealth", eng::kTypeInfo_ServerSettings, 0x2C, VarKind::Percent},
    {"vars.playerRespawnTime", eng::kTypeInfo_ServerSettings, 0x68, VarKind::Percent},
    {"vars.vehicleSpawnAllowed", eng::kTypeInfo_ServerSettings, 0xB7, VarKind::Flag},
    {"vars.vehicleSpawnDelay", eng::kTypeInfo_ServerSettings, 0x8C, VarKind::Percent},
    {"vars.hudMinimap", eng::kTypeInfo_SyncedMOHWSettings, 0x0C, VarKind::InverseFlag},
    {"vars.hudCrosshair", eng::kTypeInfo_SyncedMOHWSettings, 0x0D, VarKind::InverseFlag},
    {"vars.hudExplosiveIcons", eng::kTypeInfo_SyncedMOHWSettings, 0x0E, VarKind::InverseFlag},
    {"vars.hudHealthAmmo", eng::kTypeInfo_SyncedMOHWSettings, 0x0F, VarKind::InverseFlag},
    {"vars.hudPointsTracker", eng::kTypeInfo_SyncedMOHWSettings, 0x10, VarKind::InverseFlag},
    {"vars.hudObiturary", eng::kTypeInfo_SyncedMOHWSettings, 0x11, VarKind::InverseFlag},
    {"vars.hudEnemyTag", eng::kTypeInfo_SyncedMOHWSettings, 0x12, VarKind::InverseFlag},
    {"vars.hudGameMode", eng::kTypeInfo_SyncedMOHWSettings, 0x13, VarKind::InverseFlag},
    {"vars.hudBuddyInfo", eng::kTypeInfo_SyncedMOHWSettings, 0x14, VarKind::InverseFlag},
    {"vars.hudUnlocks", eng::kTypeInfo_SyncedMOHWSettings, 0x15, VarKind::InverseFlag},
    {"vars.hudClassAbility", eng::kTypeInfo_SyncedMOHWSettings, 0x16, VarKind::InverseFlag},
    {"vars.killCam", eng::kTypeInfo_SyncedMOHWSettings, 0x17, VarKind::InverseFlag},
    {"vars.buddyOutline", eng::kTypeInfo_SyncedMOHWSettings, 0x18, VarKind::InverseFlag},
};

struct MapName
{
    const char* code;
    const char* name;
};
const MapName kMapNames[] = {
    {"MP_03", "Somalia Stronghold"}, {"MP_05", "Novi Grad Warzone"}, {"MP_10", "Sarajevo Stadium"}, {"MP_12", "Basilan Aftermath"},
    {"MP_13", "Hara Dunes"}, {"MP_16", "Al Fara Cliffside"}, {"MP_18", "Shogore Valley"}, {"MP_19", "Tungawan Jungle"},
    {"MP_20", "Darra Gun Market"}, {"MP_21", "Chitral Compound"},
};

struct Seen
{
    std::string name;
    std::string guid;
    int team = 0;
    int squad = 0;
    void* soldier = nullptr;
    int kills = 0;
    int deaths = 0;
    int score = 0;
    bool bot = false;
    bool present = false;
};

// The network chat message your game shows: the text is an inline string (begin | 2, end, capacity end, then the
// characters), channel 4 is the game's own ADMIN line (0xF55397).
#pragma pack(push, 4)
struct ChatMessage
{
    uintptr_t begin;
    wchar_t* end;
    wchar_t* capacityEnd;
    wchar_t text[0x80];
    uint32_t playerId;
    uint32_t unknown110;
    uint32_t unknown114;
    uint32_t channel;
    uint32_t unknown11C;
};
#pragma pack(pop)
static_assert(sizeof(ChatMessage) == 0x120 && offsetof(ChatMessage, playerId) == 0x10C && offsetof(ChatMessage, channel) == 0x118, "layout");
constexpr uint32_t kChannelAdmin = 4;
using ShowChatFn = void(__stdcall*)(void* message);
ShowChatFn g_showChat = nullptr;
std::vector<std::string> g_says;  // under g_namesLock

bool g_on = false;
bool g_lockReady = false;  // g_namesLock initialized (Init with [rcon] Enabled=1)
std::string g_serverName = "mohw-bots";
std::string g_mode = "TeamDeathMatch";  // what's being played (modes::CurrentMode, taken when a level comes in)
std::unordered_map<void*, Seen> g_seen;
CRITICAL_SECTION g_namesLock;
std::unordered_map<void*, std::string> g_names;  // the message hook's thread reads these too
void* g_level = nullptr;
std::string g_map;
float g_uptime = 0.0f;
float g_roundTime = 0.0f;
float g_publishTimer = 0.0f;
float g_varsTimer = 0.0f;
bool g_roundWasOver = false;
float g_restartIn = -1.0f;  // seconds to ProCon's Next / Restart Round; -1 none asked
modes::Entry g_restartTarget;
bool g_restartSame = false;
int g_restartNextAfter = -1;
bool g_inPlace = true;  // [rcon] NextRoundInPlace: load the level in place (else, or if that fails, restart the game)
// [launch] NextRoundSeconds: after a round ends with you still in the game, the next round (as ProCon's Next Round) comes
// this long after it. Offline MOHW never moves on by itself: the results screen stayed up until you quit. 0 = stay there.
int g_nextRoundSeconds = 30;
float g_nextRoundIn = -1.0f;  // counting down to it; -1 none

void LogLine(const char* text) { Log("%s", text); }

bool ShowChat(ChatMessage* message)
{
    __try
    {
        g_showChat(message);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::string Narrow(const wchar_t* text)
{
    char out[512] = {};
    WideCharToMultiByte(1252, 0, text, -1, out, sizeof(out) - 1, nullptr, nullptr);
    return out;
}

std::string IniString(const wchar_t* key, const wchar_t* fallback, const wchar_t* section = L"rcon")
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    wchar_t text[256] = {};
    GetPrivateProfileStringW(section, key, fallback, text, 256, path);
    return Narrow(text);
}

// A made-up id of the shape ProCon shows: "EA_" and 32 hex digits, the same for a name every time.
std::string GuidFor(const std::string& name)
{
    uint64_t a = 0xCBF29CE484222325ull, b = 0x84222325CBF29CE4ull;
    for (char c : name)
    {
        a = (a ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
        b = (b * 0x100000001B3ull) ^ static_cast<unsigned char>(c);
    }
    char text[40];
    std::snprintf(text, sizeof(text), "EA_%016llX%016llX", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return text;
}

bool ReadName(void* player, std::string* out)
{
    char name[33] = {};
    if (!player || !eng::ReadBytes(eng::At(player, kPlayer_InlineName), name, sizeof(name) - 1) || !name[0])
        return false;
    for (const char* c = name; *c; ++c)
        if (static_cast<unsigned char>(*c) < 0x20 || static_cast<unsigned char>(*c) >= 0x7F)
            return false;
    *out = name;
    return true;
}

std::string HumanName(void* player)
{
    std::string name;
    if (ReadName(player, &name))
        return name;
    void* local = nullptr;
    if (eng::ClientLocalPlayer(&local) && ReadName(local, &name))
        return name;
    return "you";
}

void AppendPlayer(rconsrv::Words& words, const Seen& p)
{
    words.insert(words.end(), {p.name, p.guid, std::to_string(p.team), std::to_string(p.squad), std::to_string(p.kills),
                               std::to_string(p.deaths), std::to_string(p.score)});
}

void AppendHeader(rconsrv::Words& words, size_t count)
{
    words.insert(words.end(), {"7", "name", "guid", "teamId", "squadId", "kills", "deaths", "score", std::to_string(count)});
}

void PublishVars()
{
    for (const SettingVar& var : kSettingVars)
    {
        void* container = nullptr;
        if (!eng::GetSettingsContainer(var.typeInfo, &container) || !container)
            continue;
        std::string value;
        if (var.kind == VarKind::Percent)
        {
            float number = 0.0f;
            if (!eng::ReadFloat(eng::At(container, var.offset), &number))
                continue;
            value = std::to_string(static_cast<int>(std::lround(number * 100.0f)));
        }
        else
        {
            uint8_t flag = 0;
            if (!eng::ReadByte(eng::At(container, var.offset), &flag))
                continue;
            value = (var.kind == VarKind::Flag ? flag != 0 : flag == 0) ? "true" : "false";
        }
        rconsrv::SetVar(var.name, value, true);
    }
    rconsrv::SetVar("vars.serverName", g_serverName, true);
    rconsrv::SetVar("vars.preset", hardcore::Enabled() ? "hardcore" : "normal", true);
    rconsrv::SetVar("vars.hardCore", hardcore::Enabled() ? "true" : "false", true);
}

void SetSettingVar(const SettingVar& var, const std::string& value)
{
    void* container = nullptr;
    if (!eng::GetSettingsContainer(var.typeInfo, &container) || !container)
    {
        Log("rcon: %s: the settings aren't there yet", var.name);
        return;
    }
    char* field = static_cast<char*>(container) + var.offset;
    if (var.kind == VarKind::Percent)
    {
        float number = static_cast<float>(std::atof(value.c_str())) / 100.0f;
        if (number >= 0.0f)
            eng::WriteBytes(field, &number, sizeof(number));
    }
    else
    {
        bool on = value == "true" || value == "1";
        uint8_t flag = (var.kind == VarKind::Flag ? on : !on) ? 1 : 0;
        eng::WriteBytes(field, &flag, 1);
    }
    Log("rcon: %s set to %s", var.name, value.c_str());
    hardcore::Release(var.typeInfo, var.offset);
}

void* PlayerNamed(const std::string& name)
{
    for (const auto& entry : g_seen)
        if (entry.second.name == name && entry.second.present)
            return entry.first;
    return nullptr;
}

// "MP_03", "somalia" or "Somalia Stronghold".
const MapName* FindMap(const std::string& text)
{
    if (text.empty())
        return nullptr;
    for (const MapName& map : kMapNames)
        if (_stricmp(text.c_str(), map.code) == 0)
            return &map;
    for (const MapName& map : kMapNames)
    {
        std::string name = map.name;
        for (size_t at = 0; at + text.size() <= name.size(); ++at)
            if (_strnicmp(name.c_str() + at, text.c_str(), text.size()) == 0)
                return &map;
    }
    return nullptr;
}

void Tell(const std::string& text)
{
    Log("rcon: %s", text.c_str());
    if (!g_lockReady)
        return;  // [rcon] Enabled=0: no chat, and no lock to take
    rconsrv::Event({"player.onChat", "Server", text});
    EnterCriticalSection(&g_namesLock);
    if (g_says.size() < 16)
        g_says.push_back(text);
    LeaveCriticalSection(&g_namesLock);
}

}  // namespace

void Announce(const std::string& text) { Tell(text); }

namespace {

void SwitchHardcore(bool on)
{
    if (hardcore::Set(on))
        Tell(on ? "Hardcore mode on. Soldier health changes at the next spawn." : "Hardcore mode off. Soldier health changes at the next spawn.");
    PublishVars();
}

// ProCon's Next Round / Restart Round: say what comes, then (a moment later, so the chat line and ProCon's answer get
// out) load it in place (levelchange.h), or restart the game with it if that's off or can't be asked for. Next Round plays
// the rotation's next entry (NextMap, which ProCon's map list keeps up to date) and moves the turn on; Restart Round
// plays this map and mode again.
constexpr float kInPlaceDelay = 1.5f;
constexpr float kRestartDelay = 3.0f;

void AskRestart(bool sameMap)
{
    if (g_restartIn >= 0.0f || levelchange::Pending())
    {
        Tell("A map change is under way already");
        return;
    }
    wchar_t ini[MAX_PATH], state[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    swprintf(state, MAX_PATH, L"%smohw-bots-state.ini", ModuleDir());
    modes::Entry entry;
    int nextAfter = -1;
    if (sameMap)
    {
        entry = {modes::CurrentMap(), modes::CurrentMode()};
        if (entry.map.empty())
        {
            Tell("Restart round: no map is being played");
            return;
        }
    }
    else
    {
        std::vector<modes::Entry> rotation = modes::ReadRotation(ini);
        if (rotation.empty())
        {
            Tell("Next round: the map list is empty; add a map in ProCon's Maplist tab first");
            return;
        }
        const int count = static_cast<int>(rotation.size());
        const int next = static_cast<int>(GetPrivateProfileIntW(L"launch", L"NextMap", 0, state)) % count;
        entry = rotation[static_cast<size_t>(next)];
        nextAfter = (next + 1) % count;
    }
    const char* name = modes::MapName(entry.map);
    Tell(std::string(sameMap ? "Restarting " : "Next round: ") + (name ? name : entry.map.c_str()) + ", " + modes::DisplayName(entry.mode) +
         (g_inPlace ? ". Loading it now." : ". The game closes and starts again with it in a few seconds."));
    g_restartTarget = entry;
    g_restartSame = sameMap;
    g_restartNextAfter = nextAfter;
    g_restartIn = g_inPlace ? kInPlaceDelay : kRestartDelay;
}

void ResetRoutes(const rconsrv::Words& w)
{
    std::string asked;
    for (size_t i = 1; i < w.size(); ++i)
        asked += (i > 1 ? " " : "") + w[i];
    const MapName* map = FindMap(asked.empty() ? g_map : asked);
    if (!map)
    {
        Tell("nav.reset: no map called \"" + asked + "\"");
        return;
    }
    bool current = false;
    int files = nav::ResetMap(map->code, &current);
    if (!current && !files)
        Tell(std::string(map->name) + " has no learned routes to reset");
    else
        Tell(std::string("Bot routes reset for ") + map->name + (current ? ": the bots are learning it again now" : ": the bots learn it again next time it's played"));
}

void RunCommand(const rconsrv::Words& w)
{
    const std::string& command = w[0];
    if (command == "admin.killPlayer" && w.size() > 1)
    {
        void* player = PlayerNamed(w[1]);
        void* soldier = nullptr;
        if (player)
            eng::ReadPtr(eng::At(player, eng::kPlayer_Soldier), &soldier);
        bool killed = soldier && eng::KillSoldier(soldier);
        Log("rcon: admin.killPlayer %s: %s", w[1].c_str(), killed ? "killed" : "no soldier to kill");
    }
    else if (command == "admin.kickPlayer" && w.size() > 1)
    {
        void* player = PlayerNamed(w[1]);
        bool kicked = player && bots::Kick(player);
        Log("rcon: admin.kickPlayer %s: %s", w[1].c_str(), kicked ? "goes at its next death (a new bot fills its place)" : "not a bot of ours");
    }
    else if ((command == "admin.say" || command == "admin.yell") && w.size() > 1)
    {
        Log("rcon: %s: %s", command.c_str(), w[1].c_str());
        EnterCriticalSection(&g_namesLock);
        if (g_says.size() < 16)
            g_says.push_back(w[1]);
        LeaveCriticalSection(&g_namesLock);
        rconsrv::Event({"player.onChat", "Server", w[1]});
    }
    // ProCon changed the map list (add, remove, clear, next map): rconsrv sends the whole of it to save.
    else if (command == "mapList.persist" && w.size() >= 2)
    {
        std::vector<modes::Entry> rotation;
        for (size_t i = 2; i + 1 < w.size(); i += 2)
            rotation.push_back({w[i], w[i + 1]});
        wchar_t ini[MAX_PATH], state[MAX_PATH];
        swprintf(ini, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
        swprintf(state, MAX_PATH, L"%smohw-bots-state.ini", ModuleDir());
        const bool saved = modes::WriteRotation(ini, rotation);
        wchar_t next[16];
        swprintf(next, 16, L"%d", std::atoi(w[1].c_str()));
        WritePrivateProfileStringW(L"launch", L"NextMap", next, state);
        const int index = std::atoi(w[1].c_str());
        const bool valid = index >= 0 && index < static_cast<int>(rotation.size());
        Log("rcon: map list %s: [launch] Map=%s; next launch: %s%s%s", saved ? "saved in mohw-bots.ini" : "NOT saved (mohw-bots.ini)",
            modes::FormatRotation(rotation, "").c_str(), valid ? rotation[index].map.c_str() : "nothing (empty list)",
            valid ? ", " : "", valid ? modes::DisplayName(rotation[index].mode) : "");
    }
    else if (command == "mapList.runNextRound" || command == "mapList.restartRound")
    {
        AskRestart(command == "mapList.restartRound");
    }
    else if (command == "vars.preset" && w.size() > 1)
    {
        const bool on = _stricmp(w[1].c_str(), "hardcore") == 0;
        if (!on && _stricmp(w[1].c_str(), "normal") != 0)
        {
            Tell("vars.preset: \"" + w[1] + "\" isn't a preset; it's hardcore or normal");
            PublishVars();
        }
        else
            SwitchHardcore(on);
    }
    // The Hardcore box on ProCon's Server Settings > Gameplay page sends this.
    else if (command == "vars.hardCore" && w.size() > 1)
    {
        SwitchHardcore(w[1] == "true" || w[1] == "1");
    }
    else if (command == "nav.reset")
    {
        ResetRoutes(w);
    }
    else if (command == "vars.serverName" && w.size() > 1)
    {
        g_serverName = w[1];
    }
    else if (w.size() > 1)
    {
        for (const SettingVar& var : kSettingVars)
            if (command == var.name)
                SetSettingVar(var, w[1]);
    }
}

// A new level: the map code from "MOHW/Levels/MP/MP_03/MP_03".
void CheckLevel(void* context)
{
    void* level = nullptr;
    if (!eng::ReadPtr(eng::At(context, eng::kCtx_Level), &level) || !level || level == g_level)
        return;
    void* namePtr = nullptr;
    char name[96] = {};
    if (!eng::ReadPtr(eng::At(level, eng::kLevel_Name), &namePtr) || !namePtr || !eng::ReadBytes(namePtr, name, sizeof(name) - 1) || !name[0])
        return;
    g_level = level;
    const char* slash = std::strrchr(name, '/');
    g_map = slash ? slash + 1 : name;
    g_mode = modes::CurrentMode();
    g_seen.clear();
    g_roundTime = 0.0f;
    g_roundWasOver = false;
    g_nextRoundIn = -1.0f;
    Log("rcon: level %s (%s)", g_map.c_str(), modes::DisplayName(g_mode));
    rconsrv::Event({"server.onLevelLoaded", g_map, g_mode, "0", "1"});
    rconsrv::Event({"server.onLevelStarted"});
}

// rconsrv's check for mapList.add: a map these bots know, with that mode.
const char* CheckMap(std::string& map, std::string& mode)
{
    std::string canonicalMap = modes::CanonicalMap(map);
    if (canonicalMap.empty())
        return "InvalidMap";
    std::string canonicalMode = modes::Canonical(mode);
    if (canonicalMode.empty() || !modes::MapHasMode(canonicalMap, canonicalMode))
        return "InvalidGameModeOnMap";
    map = canonicalMap;
    mode = canonicalMode;
    return nullptr;
}

}  // namespace

void Init()
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    if (GetPrivateProfileIntW(L"rcon", L"Enabled", 1, path) == 0)
    {
        Log("rcon: off (Enabled=0)");
        return;
    }
    InitializeCriticalSection(&g_namesLock);
    g_lockReady = true;
    rconsrv::Config config;
    config.port = static_cast<int>(GetPrivateProfileIntW(L"rcon", L"Port", 47200, path));
    config.bind = IniString(L"Bind", L"127.0.0.1");
    config.password = IniString(L"Password", L"");
    config.log = &LogLine;
    config.checkMap = &CheckMap;
    g_serverName = IniString(L"ServerName", L"mohw-bots");
    g_inPlace = GetPrivateProfileIntW(L"rcon", L"NextRoundInPlace", 1, path) != 0;
    Log("rcon: Next Round / Restart Round %s", g_inPlace ? "load the level in place (restarting the game if that fails)"
                                                         : "restart the game ([rcon] NextRoundInPlace=0)");
    g_nextRoundSeconds = static_cast<int>(GetPrivateProfileIntW(L"launch", L"NextRoundSeconds", 30, path));
    if (g_nextRoundSeconds > 0)
        Log("rcon: a round that ends with you in the game moves on to the next map %d s later", g_nextRoundSeconds);
    else
        Log("rcon: a round that ends stays on the results screen ([launch] NextRoundSeconds=0)");
    g_mode = modes::CurrentMode();

    // Read only, nothing behind them here; ProCon asks for all of these when it connects.
    for (const char* name : {"vars.serverDescription", "vars.serverMessage", "vars.bannerUrl", "vars.gamePassword"})
        rconsrv::SetVar(name, "", false);
    for (const char* name : {"vars.3pCam", "vars.autoBalance", "vars.ranked"})
        rconsrv::SetVar(name, "false", false);
    for (const char* name : {"vars.idleTimeout", "vars.idleBanRounds", "vars.roundStartPlayerCount", "vars.roundRestartPlayerCount",
                             "vars.teamKillCountForKick", "vars.teamKillValueForKick", "vars.teamKillValueIncrease",
                             "vars.teamKillValueDecreasePerSecond", "vars.maxPlayers"})
        rconsrv::SetVar(name, "0", false);
    rconsrv::SetVar("vars.serverName", g_serverName, true);
    rconsrv::SetVar("vars.preset", hardcore::Enabled() ? "hardcore" : "normal", true);
    rconsrv::SetVar("vars.hardCore", hardcore::Enabled() ? "true" : "false", true);
    // Until the settings exist (ProCon may connect while the game starts up, and asks only once).
    for (const SettingVar& var : kSettingVars)
        rconsrv::SetVar(var.name, var.kind == VarKind::Percent ? "100" : var.kind == VarKind::InverseFlag ? "true" : "false", true);
    g_showChat = reinterpret_cast<ShowChatFn>(eng::Address("ClientChat_show"));

    rconsrv::Snapshot first;
    first.serverName = hardcore::Enabled() ? g_serverName + " [Hardcore]" : g_serverName;
    first.map = modes::CurrentMap();
    first.mode = g_mode;
    rconsrv::Publish(first);
    // The rotation: ProCon's Maplist tab edits it from here on (saved to [launch] Map for the next launch).
    std::vector<rconsrv::MapEntry> maps;
    for (const modes::Entry& e : modes::ReadRotation(path))
        maps.push_back({e.map, e.mode, 1});
    wchar_t state[MAX_PATH];
    swprintf(state, MAX_PATH, L"%smohw-bots-state.ini", ModuleDir());
    rconsrv::SetMapList(maps, static_cast<int>(GetPrivateProfileIntW(L"launch", L"NextMap", 0, state)));
    g_on = rconsrv::Start(config);
}

void ClientTick()
{
    levelchange::ClientTick();
    if (!g_on || !g_showChat)
        return;
    std::vector<std::string> says;
    EnterCriticalSection(&g_namesLock);
    says.swap(g_says);
    LeaveCriticalSection(&g_namesLock);
    for (const std::string& say : says)
    {
        static ChatMessage message;
        std::memset(&message, 0, sizeof(message));
        int length = MultiByteToWideChar(1252, 0, say.c_str(), static_cast<int>(say.size()), message.text, 0x7E);
        message.begin = reinterpret_cast<uintptr_t>(message.text) | 2;
        message.end = message.text + length;
        message.capacityEnd = message.text + 0x7F;
        message.channel = kChannelAdmin;
        if (!ShowChat(&message))
            Log("rcon: showing admin chat on your HUD failed (exception)");
    }
}

void OnMessage(const void* message)
{
    if (!g_on)
        return;
    const uint32_t* header = static_cast<const uint32_t*>(message);
    if (header[2] != kServerPlayerChat || header[1] != kCategoryServerPlayer)
        return;
    void* player = nullptr;
    void* text = nullptr;
    if (!eng::ProbePtr(eng::At(message, kChat_Player), &player) || !eng::ProbePtr(eng::At(message, kChat_Text), &text) || !text)
        return;
    wchar_t wide[256] = {};
    if (!eng::ReadBytes(text, wide, sizeof(wide) - sizeof(wchar_t)))
        return;
    std::string name = "Server";
    EnterCriticalSection(&g_namesLock);
    auto known = g_names.find(player);
    if (known != g_names.end())
        name = known->second;
    LeaveCriticalSection(&g_namesLock);
    rconsrv::Event({"player.onChat", name, Narrow(wide)});
}

void Tick(void* context, const std::vector<void*>& players, BotNameFn botName, const std::vector<scoring::Kill>& kills,
          bool roundOver, float dt)
{
    if (!g_on)
        return;
    g_uptime += dt;
    if (!roundOver)
        g_roundTime += dt;
    CheckLevel(context);

    for (auto& entry : g_seen)
        entry.second.present = false;
    for (void* p : players)
    {
        const char* bot = botName(p);
        auto found = g_seen.find(p);
        bool isNew = found == g_seen.end();
        Seen& seen = g_seen[p];
        uint8_t ai = 0;
        eng::ReadByte(eng::At(p, eng::kPlayer_IsAI), &ai);
        char otherAi[24];
        std::snprintf(otherAi, sizeof(otherAi), "AI_%p", p);
        std::string name = bot ? std::string(bot) : ai ? std::string(otherAi) : HumanName(p);
        if (isNew || seen.name != name)
        {
            seen.name = name;
            seen.guid = GuidFor(name);
            EnterCriticalSection(&g_namesLock);
            g_names[p] = name;
            LeaveCriticalSection(&g_namesLock);
        }
        if (isNew)
        {
            if (!bot && !ai)
                Log("rcon: you show in ProCon as \"%s\"", seen.name.c_str());
            rconsrv::Event({"player.onJoin", seen.name, seen.guid});
            rconsrv::Event({"player.onAuthenticated", seen.name, seen.guid});
        }
        seen.present = true;
        seen.bot = bot != nullptr;

        uint32_t team = 0;
        eng::ReadU32(eng::At(p, eng::kPlayer_Team), &team);
        int squad = 0;
        int rawSquad = eng::kNoSquad;
        if (eng::GetSquad(p, &rawSquad) && rawSquad >= 0 && rawSquad != eng::kNoSquad)
            squad = rawSquad + 1;
        if (!isNew && static_cast<int>(team) != seen.team)
            rconsrv::Event({"player.onTeamChange", seen.name, std::to_string(team), std::to_string(squad)});
        else if (!isNew && squad != seen.squad)
            rconsrv::Event({"player.onSquadChange", seen.name, std::to_string(team), std::to_string(squad)});
        seen.team = static_cast<int>(team);
        seen.squad = squad;

        void* soldier = nullptr;
        eng::ReadPtr(eng::At(p, eng::kPlayer_Soldier), &soldier);
        if (soldier && soldier != seen.soldier)
            rconsrv::Event({"player.onSpawn", seen.name, std::to_string(team)});
        seen.soldier = soldier;
        scoreboard::ReadRow(p, &seen.kills, &seen.deaths, &seen.score);
    }
    for (auto it = g_seen.begin(); it != g_seen.end();)
    {
        if (it->second.present)
        {
            ++it;
            continue;
        }
        rconsrv::Words leave = {"player.onLeave", it->second.name};
        AppendHeader(leave, 1);
        AppendPlayer(leave, it->second);
        rconsrv::Event(leave);
        EnterCriticalSection(&g_namesLock);
        g_names.erase(it->first);
        LeaveCriticalSection(&g_namesLock);
        it = g_seen.erase(it);
    }

    for (const scoring::Kill& kill : kills)
    {
        static int weaponsLogged = 0;
        if (weaponsLogged < 8)
        {
            ++weaponsLogged;
            Log("rcon: a kill's weapon reads \"%s\" (first 8 logged)", kill.weapon);
        }
        auto killer = g_seen.find(kill.killer);
        auto victim = g_seen.find(kill.victim);
        if (victim == g_seen.end())
            continue;
        rconsrv::Event({"player.onKill", killer != g_seen.end() ? killer->second.name : std::string(), victim->second.name,
                        kill.weapon[0] ? kill.weapon : "Death", kill.headshot ? "true" : "false"});
    }

    for (const rconsrv::Words& command : rconsrv::TakeCommands())
        if (!command.empty())
            RunCommand(command);
    bool humanIn = false;  // you, still in the game (a kick or leaving ends the round with nobody: your game restarts it)
    for (void* p : players)
    {
        uint8_t ai = 1;
        if (!botName(p) && eng::ReadByte(eng::At(p, eng::kPlayer_IsAI), &ai) && !ai)
            humanIn = true;
    }
    if (g_nextRoundIn >= 0.0f)
    {
        g_nextRoundIn -= dt;
        if (!roundOver || !humanIn || g_restartIn >= 0.0f || levelchange::Pending())
        {
            Log("rcon: the next round isn't needed any more (%s)", !roundOver ? "a new round started" : !humanIn ? "you left the game"
                                                                                                  : "a map change is under way");
            g_nextRoundIn = -1.0f;
        }
        else if (g_nextRoundIn < 0.0f)
        {
            Log("rcon: the round ended %d s ago; next round", g_nextRoundSeconds);
            AskRestart(false);
        }
    }
    if (g_restartIn >= 0.0f)
    {
        g_restartIn -= dt;
        if (g_restartIn < 0.0f && !(g_inPlace && levelchange::Request(g_restartTarget, g_restartSame, g_restartNextAfter)))
        {
            nav::OnLevel(std::string());  // saves the routes learned since the last save
            Log("rcon: restarting the game for ProCon's %s", g_restartSame ? "Restart Round" : "Next Round");
            if (g_inPlace)
                Tell("Loading it in place didn't work; the game restarts with it instead.");
            levelchange::RestartWith(g_restartTarget, g_restartNextAfter);
        }
    }

    int teamScores[2] = {0, 0};
    for (const auto& entry : g_seen)
        if (entry.second.team == 1 || entry.second.team == 2)
            teamScores[entry.second.team - 1] += entry.second.kills;

    if (roundOver != g_roundWasOver)
    {
        g_roundWasOver = roundOver;
        if (roundOver)
        {
            rconsrv::Words rows = {"server.onRoundOverPlayers"};
            AppendHeader(rows, g_seen.size());
            for (const auto& entry : g_seen)
                AppendPlayer(rows, entry.second);
            rconsrv::Event(rows);
            rconsrv::Event({"server.onRoundOverTeamScores", "2", std::to_string(teamScores[0]), std::to_string(teamScores[1]), "0"});
            int winner = teamScores[0] > teamScores[1] ? 1 : teamScores[1] > teamScores[0] ? 2 : 0;
            rconsrv::Event({"server.onRoundOver", std::to_string(winner)});
            Log("rcon: round over told to ProCon (%d - %d)", teamScores[0], teamScores[1]);
            if (g_nextRoundSeconds > 0 && humanIn && g_restartIn < 0.0f && !levelchange::Pending())
            {
                g_nextRoundIn = static_cast<float>(g_nextRoundSeconds);
                Tell("Next round in " + std::to_string(g_nextRoundSeconds) + " seconds.");
            }
            else if (g_nextRoundSeconds > 0 && !humanIn)
                Log("rcon: the round ended with no human in the game (a kick, or you left): your game starts the next one itself");
        }
        else
        {
            g_nextRoundIn = -1.0f;
            g_roundTime = 0.0f;
            rconsrv::Event({"server.onLevelStarted"});
        }
    }

    g_varsTimer -= dt;
    if (g_varsTimer <= 0.0f)
    {
        g_varsTimer = 2.0f;
        PublishVars();
    }
    g_publishTimer -= dt;
    if (g_publishTimer > 0.0f)
        return;
    g_publishTimer = 0.5f;

    rconsrv::Snapshot s;
    s.serverName = hardcore::Enabled() ? g_serverName + " [Hardcore]" : g_serverName;  // the ProCon plugin reads this
    s.map = g_map;
    s.mode = g_mode;
    s.teamScores[0] = teamScores[0];
    s.teamScores[1] = teamScores[1];
    s.roundSeconds = static_cast<int>(g_roundTime);
    s.uptimeSeconds = static_cast<int>(g_uptime);
    void* manager = nullptr;
    uint32_t maxPlayers = 0;
    if (eng::ReadPtr(eng::At(context, eng::kCtx_PlayerManager), &manager) && manager &&
        eng::ReadU32(eng::At(manager, eng::kMgr_MaxPlayers), &maxPlayers) && maxPlayers)
    {
        s.maxPlayers = static_cast<int>(maxPlayers);
        rconsrv::SetVar("vars.maxPlayers", std::to_string(maxPlayers), false);
    }
    void* bf = nullptr;
    float modifier = 1.0f;
    if (g_mode == "TeamDeathMatch" && eng::GetSettingsContainer(eng::kTypeInfo_SyncedBFSettings, &bf) && bf &&
        eng::ReadFloat(eng::At(bf, 0x0C), &modifier))
        s.targetScore = static_cast<int>(std::lround(modifier * kTdmBaseKills));
    for (const auto& entry : g_seen)
    {
        rconsrv::Player row;
        row.name = entry.second.name;
        row.guid = entry.second.guid;
        row.team = entry.second.team;
        row.squad = entry.second.squad;
        row.kills = entry.second.kills;
        row.deaths = entry.second.deaths;
        row.score = entry.second.score;
        row.bot = entry.second.bot;
        s.players.push_back(row);
    }
    rconsrv::Publish(s);
}

}  // namespace rcon
