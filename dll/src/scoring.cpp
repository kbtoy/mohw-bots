#include "scoring.h"

#include <windows.h>

#include <cstdint>
#include <cwchar>

#include "engine.h"
#include "log.h"

namespace scoring {
namespace {

// ServerPlayerKilledMessage (category ServerPlayer 0xF3C93DD3, type 0xC7ABB9DB), built by 0xE41A40:
// +0x28 victim, +0x2C damage giver info, +0x30 killer (giver's player), +0x34 weapon name, +0x54 road kill,
// +0x55 headshot (hit body part 1), +0x56 / +0x58 from the death info.
constexpr uint32_t kCategoryServerPlayer = 0xF3C93DD3;
constexpr uint32_t kServerPlayerKilled = 0xC7ABB9DB;
constexpr int kKilled_Victim = 0x28;
constexpr int kKilled_Killer = 0x30;
constexpr int kKilled_Weapon = 0x34;
constexpr int kKilled_Headshot = 0x55;

// StatEvent ids of the ScoringTypeData entries used (Measuring), for the message's +0x40.
constexpr int kStatEventKill = 4;
constexpr int kStatEventHeadshot = 71;

// StatTokenPointsMessage (category Stat 0x7C89B9B7, ctor 0x2627230), sent by TokenAction_GiveXP (0xB72760).
constexpr uint32_t kCategoryStat = 0x7C89B9B7;
constexpr uint32_t kStatTokenPoints = 0xA9E33CD8;
constexpr int kToken_Player = 0x28;
constexpr int kToken_StatEvent = 0x2C;

// The ScoringTypeData entries (mohw/persistence/scoring/scoring) of every StatEvent a TokenAction_GiveXP gives in
// the shipped modes: combatmission_interact_token / _bombsitemanager_token (Combat Mission and Hot Spot),
// sport_1l_v2/sport_manager (Home Run), ctf_tokens. All bucket 0, no RequestVO.
struct TokenScoreEntry
{
    int statEvent;
    const char* what;  // for the log
    int points;
    const char* sid;
    int visibility;
    float buddyPoints;
    const char* buddySid;
    float buddyDistance;
};
constexpr TokenScoreEntry kTokenScores[] = {
    {173, "a charge armed", 200, "ID_IGHUD_SCORE_BOMB_PLANTED", 0, 100.0f, "ID_IGHUD_SCORE_BUDDY_BOMB_PLANTED", 7.0f},
    {175, "a charge disarmed", 200, "ID_IGHUD_SCORE_BOMB_DEFUSED", 0, 100.0f, "ID_IGHUD_SCORE_BUDDY_BOMB_DEFUSED", 7.0f},
    {177, "a charge went off", 50, "ID_IGHUD_SCORE_BOMB_EXPLODED", 0, 0.0f, nullptr, 20.0f},
    {178, "an objective destroyed", 100, "ID_IGHUD_SCORE_OBJECTIVE_COMPLETE", 0, 0.0f, nullptr, 20.0f},
    {188, "the flag stolen", 100, "ID_IGHUD_SCORE_SPORT_STOLE_FLAG", 0, 0.0f, nullptr, 20.0f},
    {190, "the flag brought home", 200, "ID_IGHUD_SCORE_SPORT_CAPTURED_FLAG", 0, 0.0f, nullptr, 20.0f},
    {197, "the flag carrier killed", 200, "ID_IGHUD_SCORE_SPORT_KILL_CARRIER", 0, 0.0f, nullptr, 20.0f},
    {199, "the round survived", 100, "ID_IGHUD_SCORE_SPORT_SURVIVOR", 0, 0.0f, nullptr, 20.0f},
    {200, "the flag picked up", 100, "ID_IGHUD_XP_PICKED_UP_FLAG", 0, 50.0f, "ID_IGHUD_XP_PICKED_UP_FLAG_BUDDY", 20.0f},
    {201, "a dropped flag picked up", 100, "ID_IGHUD_XP_PICKED_UP_DROPPED_FLAG", 0, 50.0f,
     "ID_IGHUD_XP_PICKED_UP_DROPPED_FLAG_BUDDY", 20.0f},
    {202, "the flag captured", 300, "ID_IGHUD_XP_CAPTURED", 0, 150.0f, "ID_IGHUD_XP_CAPTURED_FLAG_BUDDY", 20.0f},
    {204, "the flag rescued", 50, "ID_IGHUD_XP_RESCUED_FLAG", 0, 25.0f, "ID_IGHUD_XP_RESCUED_FLAG_BUDDY", 20.0f},
    {205, "the flag carrier killed", 50, "ID_IGHUD_XP_KILL_FLAG_CARRIER", 1, 25.0f, "ID_IGHUD_XP_KILL_FLAG_CARRIER_BUDDY",
     20.0f},
    {207, "a kill while holding the flag", 50, "ID_IGHUD_XP_KILLED_HOLDING_FLAG", 1, 25.0f, "Buddy kill with flag", 20.0f},
};

const TokenScoreEntry* FindTokenScore(int statEvent)
{
    for (const TokenScoreEntry& e : kTokenScores)
        if (e.statEvent == statEvent)
            return &e;
    return nullptr;
}

struct Settings
{
    bool enabled = true;
    int killPoints = 100;     // ScoringTypeData "ID_IGHUD_SCORE_KILL": Score 100, buddy bonus 10 within 20 m
    int headshotPoints = 50;  // "ID_IGHUD_SCORE_HEADSHOT": Score 50, buddy bonus 5 within 20 m
};
Settings g_settings;

CRITICAL_SECTION g_lock;
std::vector<Kill> g_kills;
std::vector<TokenEvent> g_tokenEvents;

int ReadInt(const wchar_t* key, int fallback)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    return static_cast<int>(GetPrivateProfileIntW(L"scoring", key, fallback, path));
}

bool OnlineStatsPresent()
{
    void* stats = nullptr;
    return eng::ProbePtr(reinterpret_cast<const void*>(eng::kOnlineStatsPtr), &stats) && stats;
}

}  // namespace

void Init()
{
    InitializeCriticalSection(&g_lock);
    g_settings.enabled = ReadInt(L"Enabled", 1) != 0;
    g_settings.killPoints = ReadInt(L"KillPoints", g_settings.killPoints);
    g_settings.headshotPoints = ReadInt(L"HeadshotPoints", g_settings.headshotPoints);
    Log("scoring: offline score events %s (kill %d, headshot %d; buddy bonus by the game; charges armed / disarmed and "
        "Home Run flags at the game's values, counted on the end-of-round screen)",
        g_settings.enabled ? "on" : "off", g_settings.killPoints, g_settings.headshotPoints);
}

bool Enabled() { return g_settings.enabled; }
int KillPoints() { return g_settings.killPoints; }
int HeadshotPoints() { return g_settings.headshotPoints; }

void OnMessage(const void* message)
{
    const uint32_t* header = static_cast<const uint32_t*>(message);
    if (header[2] == kStatTokenPoints && header[1] == kCategoryStat)
    {
        const char* m = static_cast<const char*>(message);
        TokenEvent event;
        event.player = *reinterpret_cast<void* const*>(m + kToken_Player);
        event.statEvent = *reinterpret_cast<const int*>(m + kToken_StatEvent);
        EnterCriticalSection(&g_lock);
        if (event.player && g_tokenEvents.size() < 256)
            g_tokenEvents.push_back(event);
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (header[2] != kServerPlayerKilled || header[1] != kCategoryServerPlayer)
        return;
    const char* m = static_cast<const char*>(message);
    Kill kill;
    kill.victim = *reinterpret_cast<void* const*>(m + kKilled_Victim);
    kill.killer = *reinterpret_cast<void* const*>(m + kKilled_Killer);
    kill.headshot = m[kKilled_Headshot] != 0;
    void* weapon = *reinterpret_cast<void* const*>(m + kKilled_Weapon);
    if (weapon)
        eng::ReadBytes(weapon, kill.weapon, sizeof(kill.weapon) - 1);
    for (char* c = kill.weapon; *c; ++c)
        if (static_cast<unsigned char>(*c) < 0x20 || static_cast<unsigned char>(*c) >= 0x7F)
        {
            kill.weapon[0] = 0;  // not text after all
            break;
        }
    uint8_t ai = 0;
    uint32_t team = 0;
    if (kill.victim && eng::ProbeByte(eng::At(kill.victim, eng::kPlayer_IsAI), &ai))
        kill.victimIsAI = ai != 0;
    if (kill.victim && eng::ProbePtr(eng::At(kill.victim, eng::kPlayer_Team), reinterpret_cast<void**>(&team)))
        kill.victimTeam = static_cast<int>(team);
    if (kill.killer && eng::ProbeByte(eng::At(kill.killer, eng::kPlayer_IsAI), &ai))
        kill.killerIsAI = ai != 0;
    if (kill.killer && eng::ProbePtr(eng::At(kill.killer, eng::kPlayer_Team), reinterpret_cast<void**>(&team)))
        kill.killerTeam = static_cast<int>(team);
    EnterCriticalSection(&g_lock);
    if (g_kills.size() < 256)
        g_kills.push_back(kill);
    LeaveCriticalSection(&g_lock);
}

std::vector<Kill> TakeKills()
{
    std::vector<Kill> kills;
    EnterCriticalSection(&g_lock);
    kills.swap(g_kills);
    LeaveCriticalSection(&g_lock);
    return kills;
}

void Award(const Kill& kill, bool forHumanBuddy)
{
    if (!g_settings.enabled || !kill.killer || kill.killer == kill.victim || (kill.killerIsAI && !forHumanBuddy) ||
        kill.killerTeam == kill.victimTeam || OnlineStatsPresent())
        return;
    // A bot killer scores nothing itself (the scoring system skips AI players); the event is for its human buddy's
    // "+10 BUDDY KILL" (within 20 m) and the buddy's share of the special-ability meter.
    bool ok = eng::SendScoreEvent(kill.killer, kStatEventKill, static_cast<float>(g_settings.killPoints),
                                  "ID_IGHUD_SCORE_KILL", 0, 10.0f, "ID_IGHUD_SCORE_BUDDY_KILL", 20.0f);
    if (ok && kill.headshot && g_settings.headshotPoints > 0)
        ok = eng::SendScoreEvent(kill.killer, kStatEventHeadshot, static_cast<float>(g_settings.headshotPoints),
                                 "ID_IGHUD_SCORE_HEADSHOT", 1, 5.0f, "ID_IGHUD_SCORE_BUDDY_HEADSHOT", 20.0f);
    static int logged = 0;
    if (!ok || logged < 6)
    {
        Log("scoring: kill by %p%s%s -> score event %s", kill.killer, kill.headshot ? " (headshot)" : "",
            forHumanBuddy ? " (a bot: for its human buddy)" : "", ok ? "sent" : "FAILED");
        ++logged;
    }
}

std::vector<TokenEvent> TakeTokenEvents()
{
    std::vector<TokenEvent> events;
    EnterCriticalSection(&g_lock);
    events.swap(g_tokenEvents);
    LeaveCriticalSection(&g_lock);
    return events;
}

bool TokenScore(int statEvent, int* points, const char** what)
{
    const TokenScoreEntry* e = FindTokenScore(statEvent);
    if (!e)
        return false;
    *points = e->points;
    *what = e->what;
    return true;
}

bool AwardToken(const TokenEvent& event)
{
    const TokenScoreEntry* e = FindTokenScore(event.statEvent);
    if (!g_settings.enabled || !e || !event.player || OnlineStatsPresent())
        return false;
    return eng::SendScoreEvent(event.player, e->statEvent, static_cast<float>(e->points), e->sid, e->visibility,
                               e->buddyPoints, e->buddySid, e->buddyDistance);
}

}  // namespace scoring
