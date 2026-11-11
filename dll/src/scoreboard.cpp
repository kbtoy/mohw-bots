#include "scoreboard.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <unordered_map>

#include "engine.h"
#include "hook.h"
#include "log.h"
#include "scoring.h"

namespace scoreboard {
namespace {

// The scoring system's ServerPlayer listener (vtable 0x281D8B4, built by 0xE62AC0; +4 = the scoring system) and its
// handler for that category (thiscall onMessage(msg), ret 4): ServerPlayerCreate -> record for a human,
// ServerPlayerDestroy -> drop a human's record, SwitchTeam / SwitchSquad -> update any record.
constexpr uintptr_t kListenerVtable = 0x281D8B4;
constexpr uint32_t kSwitchTeam = 0xDAC6CEB7;  // ServerPlayerSwitchTeamMessage
constexpr uint32_t kDestroy = 0xE0BFD57A;     // ServerPlayerDestroyMessage
constexpr int kMessage_Player = 0x28;
// Scoring system +8 / +0xC: eastl::vector of records by player id; record +0xA8: the PlayerScore.
constexpr int kScoring_RecordsBegin = 0x08;
constexpr int kScoring_RecordsEnd = 0x0C;
constexpr int kRecord_PlayerScore = 0xA8;
constexpr int kScore_Kills = 0x140;
constexpr int kScore_Deaths = 0x144;
constexpr int kScore_Points = 0x148;
constexpr int kScore_Flags = 0x164;
constexpr int kScore_Totals = 0x168;  // kills, deaths, points, flags: the match totals
constexpr int kPlayer_IsSpectator = 0x355;
// ServerRoundSubRoundOverMessage (category ServerRound): the game adds each record's round numbers to its totals.
constexpr uint32_t kCategoryRound = 0xD93DD7C2;
constexpr uint32_t kSubRoundOver = 0x42983801;

// Client: ClientGameContext +0x44 holds the scoreboard list (+4: circular list, node +8 = the ClPlayerScore). Entry
// +0x158 weak reference to its ClientPlayer ([[+0x158]] - 8), +0x164 kills, +0x168 deaths, +0x16C points - the
// server PlayerScore's fields +0x24, as the scoreboard row builder 0x16EF6A0 reads them. The row's name: an AI
// player's name string [player+0x10], else the inline name at +0x28 (0x16EF570).
constexpr int kClientCtx_Scores = 0x44;
constexpr int kClientEntry_Player = 0x158;
constexpr int kClientEntry_Kills = 0x164;
constexpr int kClientEntry_Deaths = 0x168;
constexpr int kClientEntry_Points = 0x16C;
constexpr int kClientEntry_TotalPoints = 0x194;  // the end-of-round screens' score (0x17C5E90)
constexpr int kPlayer_AiName = 0x10;
constexpr int kPlayer_InlineName = 0x28;  // the first of the Player's four online ids {u64 id; char name[24]} at +0x20
constexpr size_t kOnlineNameSize = 24;

using ListenerFn = void(__thiscall*)(void* listener, void* message);
ListenerFn g_original = nullptr;
bool g_enabled = true;
void* g_listener = nullptr;          // the last listener seen (server thread)
std::vector<void*> g_wanted;         // bot players that get a record
std::unordered_map<void*, int> g_points;  // player -> the points shown on its row (bots and you)
volatile LONG g_subRoundOver = 0;    // the game keeps the match totals itself
int g_added = 0, g_removed = 0, g_failed = 0;

int ReadInt(const wchar_t* key, int fallback)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    return static_cast<int>(GetPrivateProfileIntW(L"bots", key, fallback, path));
}

bool Wanted(void* player) { return std::find(g_wanted.begin(), g_wanted.end(), player) != g_wanted.end(); }

void* Scoring(void* listener)
{
    void* vtable = nullptr;
    void* scoring = nullptr;
    if (!listener || !eng::ProbePtr(listener, &vtable) || reinterpret_cast<uintptr_t>(vtable) != kListenerVtable ||
        !eng::ProbePtr(eng::At(listener, 4), &scoring))
        return nullptr;
    return scoring;
}

// The player's PlayerScore, or null when it has no record.
void* PlayerScore(void* scoring, void* player)
{
    int id = -1;
    void* begin = nullptr;
    void* end = nullptr;
    if (!scoring || !eng::GetPlayerId(player, &id) || id < 0 || !eng::ReadPtr(eng::At(scoring, kScoring_RecordsBegin), &begin) ||
        !eng::ReadPtr(eng::At(scoring, kScoring_RecordsEnd), &end) || !begin ||
        id >= (static_cast<char*>(end) - static_cast<char*>(begin)) / static_cast<int>(sizeof(void*)))
        return nullptr;
    void* record = nullptr;
    void* score = nullptr;
    if (!eng::ReadPtr(static_cast<void**>(begin) + id, &record) || !record ||
        !eng::ReadPtr(eng::At(record, kRecord_PlayerScore), &score))
        return nullptr;
    return score;
}

bool HasRecord(void* scoring, void* player)
{
    int id = -1;
    void* begin = nullptr;
    void* end = nullptr;
    void* record = nullptr;
    return scoring && eng::GetPlayerId(player, &id) && id >= 0 &&
           eng::ReadPtr(eng::At(scoring, kScoring_RecordsBegin), &begin) &&
           eng::ReadPtr(eng::At(scoring, kScoring_RecordsEnd), &end) && begin &&
           id < (static_cast<char*>(end) - static_cast<char*>(begin)) / static_cast<int>(sizeof(void*)) &&
           eng::ReadPtr(static_cast<void**>(begin) + id, &record) && record;
}

// A bot joining its team gets the record a human gets on joining, just before the game's own team update for it.
// Its player leaving drops the record (the game only drops a human's).
void __fastcall ListenerDetour(void* listener, void* /*edx*/, void* message)
{
    g_listener = listener;
    uint32_t type = 0;
    void* player = nullptr;
    void* scoring = Scoring(listener);
    if (scoring && eng::ReadU32(eng::At(message, 8), &type) && (type == kSwitchTeam || type == kDestroy) &&
        eng::ReadPtr(eng::At(message, kMessage_Player), &player) && player)
    {
        uint8_t ai = 0, spectator = 1;
        bool known = eng::ReadByte(eng::At(player, eng::kPlayer_IsAI), &ai) &&
                     eng::ReadByte(eng::At(player, kPlayer_IsSpectator), &spectator);
        if (type == kSwitchTeam && known && ai && !spectator && Wanted(player) && !HasRecord(scoring, player))
        {
            if (eng::AddScoreRecord(scoring, player) && HasRecord(scoring, player))
                ++g_added;
            else
                ++g_failed;
        }
        else if (type == kDestroy && known && ai && HasRecord(scoring, player))
        {
            if (eng::RemoveScoreRecord(scoring, player))
                ++g_removed;
            else
                ++g_failed;
        }
    }
    g_original(listener, message);
}

// Points: what scoring.cpp awards a human for the same kill.
int KillValue(bool headshot) { return scoring::KillPoints() + (headshot ? scoring::HeadshotPoints() : 0); }

}  // namespace

void Init()
{
    g_enabled = ReadInt(L"Scoreboard", 1) != 0;
    if (!g_enabled)
    {
        Log("scoreboard: bots get no row (Scoreboard=0)");
        return;
    }
    if (void* target = reinterpret_cast<void*>(eng::Address("Scoring_onPlayerMessage")))
        // mov eax, [esp+4]; push edi; mov edi, ecx: 7 position-independent bytes
        g_original = reinterpret_cast<ListenerFn>(InlineHook(target, 7, reinterpret_cast<const void*>(&ListenerDetour)));
    g_enabled = g_original != nullptr;
    Log("scoreboard: %s", g_enabled ? "bots get a row (name, kills, deaths, points) when they join a team"
                                    : "scoring system listener hook FAILED; bots get no row");
}

bool Enabled() { return g_enabled; }

void AddBot(void* player)
{
    if (g_enabled && player && !Wanted(player))
        g_wanted.push_back(player);
}

void ForgetBot(void* player) { g_wanted.erase(std::remove(g_wanted.begin(), g_wanted.end(), player), g_wanted.end()); }

void Reset()
{
    g_listener = nullptr;
    g_wanted.clear();
    g_points.clear();
    InterlockedExchange(&g_subRoundOver, 0);
}

void NewLevel()
{
    // The points kept for each row came from the level being left. The game starts your kills and deaths again on the
    // new level, but Tick kept writing the old points back (2026-11-10: 2,900 from Sector Control on Hot Spot's rows).
    const size_t kept = g_points.size();
    g_points.clear();
    InterlockedExchange(&g_subRoundOver, 0);  // the new level's mode decides again whether it has sub-rounds
    int zeroed = 0;
    void* scoring = Scoring(g_listener);
    void* begin = nullptr;
    void* end = nullptr;
    if (g_enabled && scoring && eng::ReadPtr(eng::At(scoring, kScoring_RecordsBegin), &begin) &&
        eng::ReadPtr(eng::At(scoring, kScoring_RecordsEnd), &end) && begin)
        for (void** r = static_cast<void**>(begin); r < static_cast<void**>(end); ++r)
        {
            // The records left are the players staying (you): the bots' went with their players.
            void* record = nullptr;
            void* score = nullptr;
            uint32_t points = 0;
            if (eng::ReadPtr(r, &record) && record && eng::ReadPtr(eng::At(record, kRecord_PlayerScore), &score) && score &&
                eng::ReadU32(eng::At(score, kScore_Points), &points) && points != 0 && eng::SetScorePoints(score, 0))
                ++zeroed;
        }
    Log("scoreboard: new level - points start from 0 (%u row(s) forgotten, %d zeroed)", static_cast<unsigned>(kept), zeroed);
}

void CreditKill(void* killer, bool headshot)
{
    if (!g_enabled || !killer)
        return;
    bool bot = Wanted(killer);
    if (!bot && !scoring::Enabled())
        return;  // your points: only while the DLL sends your score events (offline)
    void* score = PlayerScore(Scoring(g_listener), killer);
    // The game counts your kills itself, never a bot's (0xE7A6B0 skips AI killers).
    bool ok = score && (!bot || eng::AddScoreKill(score));
    if (ok)
        g_points[killer] += KillValue(headshot);
    static int logged = 0;
    if (!ok && logged < 5)
    {
        ++logged;
        Log("scoreboard: couldn't credit a kill to player %p (%s)", killer,
            !g_listener ? "no scoring listener seen yet" : !score ? "it has no record" : "engine call failed");
    }
}

void CreditPoints(void* player, int points)
{
    if (!g_enabled || !player || points == 0 || (!Wanted(player) && !scoring::Enabled()))
        return;
    if (PlayerScore(Scoring(g_listener), player))
        g_points[player] += points;
}

bool ReadObjectives(void* player, int* count, int* total)
{
    void* score = g_enabled ? PlayerScore(Scoring(g_listener), player) : nullptr;
    uint32_t now = 0, sum = 0;
    if (!score || !eng::ReadU32(eng::At(score, kScore_Flags), &now) ||
        !eng::ReadU32(eng::At(score, kScore_Totals + 12), &sum))
        return false;
    *count = static_cast<int>(now);
    *total = static_cast<int>(sum);
    return true;
}

void Tick(const std::vector<void*>& players, float dt)
{
    static float timer = 0.0f;
    timer += dt;
    if (!g_enabled || timer < 0.25f)
        return;
    timer = 0.0f;
    void* scoring = Scoring(g_listener);
    bool keepTotals = !g_subRoundOver;
    for (void* p : players)
    {
        void* score = PlayerScore(scoring, p);
        if (!score)
            continue;
        // Points: the game recomputes yours from its per-class buckets on every score event (0xE67190), and offline
        // nothing reaches those, so it kept setting 0. Ours win, re-applied after it.
        auto points = g_points.find(p);
        uint32_t shown = 0;
        if (points != g_points.end() && eng::ReadU32(eng::At(score, kScore_Points), &shown) &&
            static_cast<int>(shown) != points->second)
            eng::SetScorePoints(score, points->second);
        // Match totals = this round's numbers (TDM is one round and never sends the sub-round end that adds them).
        uint32_t now[4] = {}, totals[4] = {};
        const int current[4] = {kScore_Kills, kScore_Deaths, kScore_Points, kScore_Flags};
        bool differ = false;
        for (int i = 0; keepTotals && i < 4; ++i)
        {
            eng::ReadU32(eng::At(score, current[i]), &now[i]);
            eng::ReadU32(eng::At(score, kScore_Totals + 4 * i), &totals[i]);
            differ |= now[i] != totals[i];
        }
        if (differ)
            eng::ClearScoreTotals(score) && eng::AddScoreToTotals(score);
    }
}

void OnMessage(const void* message)
{
    const uint32_t* header = static_cast<const uint32_t*>(message);
    if (!g_enabled || header[2] != kSubRoundOver || header[1] != kCategoryRound)
        return;
    // A mode with sub-rounds: from now on the game keeps the totals - it is about to add this round's numbers, so
    // the copies made so far go (server thread: its round messages are dispatched there).
    InterlockedExchange(&g_subRoundOver, 1);
    void* scoring = Scoring(g_listener);
    void* begin = nullptr;
    void* end = nullptr;
    if (!scoring || !eng::ReadPtr(eng::At(scoring, kScoring_RecordsBegin), &begin) ||
        !eng::ReadPtr(eng::At(scoring, kScoring_RecordsEnd), &end) || !begin)
        return;
    for (void** r = static_cast<void**>(begin); r < static_cast<void**>(end); ++r)
    {
        void* record = nullptr;
        void* score = nullptr;
        if (eng::ReadPtr(r, &record) && record && eng::ReadPtr(eng::At(record, kRecord_PlayerScore), &score) && score)
            eng::ClearScoreTotals(score);
    }
    Log("scoreboard: sub-round over - the game keeps the match totals from here");
}

void ClientTick()
{
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (!g_enabled || now - last < 1000)
        return;
    last = now;
    // The "killed by" card (0x17D4FA0) and other panels show a player's online name (Player +0x28, char[24]) - set
    // only for human players. An AI player's name is the string at +0x10 (what the scoreboard shows): copy it over.
    void* manager = eng::ClientPlayerManager();
    void* begin = nullptr;
    void* end = nullptr;
    if (!manager || !eng::ReadPtr(eng::At(manager, 0x98), &begin) || !eng::ReadPtr(eng::At(manager, 0x9C), &end) ||
        !begin || end < begin)
        return;
    for (void** it = static_cast<void**>(begin); it < static_cast<void**>(end) && it < static_cast<void**>(begin) + 64; ++it)
    {
        void* player = nullptr;
        uint8_t ai = 0;
        void* source = nullptr;
        char name[kOnlineNameSize] = {};
        char shown[kOnlineNameSize] = {};
        if (!eng::ReadPtr(it, &player) || !player || !eng::ReadByte(eng::At(player, eng::kPlayer_IsAI), &ai) || !ai ||
            !eng::ReadPtr(eng::At(player, kPlayer_AiName), &source) || !source)
            continue;
        for (size_t i = 0; i + 1 < sizeof(name); ++i)
        {
            uint8_t c = 0;
            if (!eng::ProbeByte(static_cast<char*>(source) + i, &c) || !c)
                break;
            name[i] = static_cast<char>(c);
        }
        if (!name[0] || !eng::ReadBytes(eng::At(player, kPlayer_InlineName), shown, sizeof(shown)) ||
            std::memcmp(name, shown, sizeof(name)) == 0)
            continue;
        eng::WriteBytes(static_cast<char*>(player) + kPlayer_InlineName, name, sizeof(name));
    }
}

bool ReadRow(void* player, int* kills, int* deaths, int* points)
{
    if (!g_enabled || !g_listener)
        return false;
    void* score = PlayerScore(Scoring(g_listener), player);
    uint32_t k = 0, d = 0, p = 0;
    if (!score || !eng::ReadU32(eng::At(score, kScore_Kills), &k) || !eng::ReadU32(eng::At(score, kScore_Deaths), &d) ||
        !eng::ReadU32(eng::At(score, kScore_Points), &p))
        return false;
    *kills = static_cast<int>(k);
    *deaths = static_cast<int>(d);
    *points = static_cast<int>(p);
    return true;
}

void LogRows(const std::vector<void*>& players, NameFn nameOf, float dt)
{
    static float timer = 0.0f;
    static int lines = 0;
    timer += dt;
    if (!g_enabled || timer < 60.0f || lines >= 20)
        return;
    timer = 0.0f;
    ++lines;
    void* scoring = Scoring(g_listener);
    char text[1200] = "";
    size_t used = 0;
    int rows = 0, missing = 0;
    for (void* p : players)
    {
        const char* name = nameOf(p);
        void* score = PlayerScore(scoring, p);
        if (!score)
        {
            ++missing;
            continue;
        }
        uint32_t kills = 0, deaths = 0, points = 0, total = 0;
        eng::ReadU32(eng::At(score, kScore_Kills), &kills);
        eng::ReadU32(eng::At(score, kScore_Deaths), &deaths);
        eng::ReadU32(eng::At(score, kScore_Points), &points);
        eng::ReadU32(eng::At(score, kScore_Totals + 8), &total);
        ++rows;
        if (used + 56 < sizeof(text))
            used += std::snprintf(text + used, sizeof(text) - used, "%s%s %u/%u/%u (%u)", rows > 1 ? ", " : "",
                                  name ? name : "?", kills, deaths, points, total);
    }
    Log("scoreboard (server, kills/deaths/points (match total points)): %d rows, %d players without one; %s; records added %d, removed "
        "%d, failed %d", rows, missing, text, g_added, g_removed, g_failed);
}

void ClientDiagnostics()
{
    static DWORD last = 0;
    static int lines = 0;
    DWORD now = GetTickCount();
    if (!g_enabled || now - last < 60000 || lines >= 20)
        return;
    last = now;
    void* context = nullptr;
    void* scores = nullptr;
    if (!eng::ProbePtr(reinterpret_cast<const void*>(eng::kClientGameContextPtr), &context) || !context ||
        !eng::ProbePtr(eng::At(context, kClientCtx_Scores), &scores) || !scores)
        return;
    ++lines;
    const void* head = eng::At(scores, 4);
    void* node = nullptr;
    char text[1200] = "";
    size_t used = 0;
    int rows = 0;
    if (eng::ReadPtr(head, &node))
        for (int guard = 0; node && node != head && guard < 64; ++guard)
        {
            void* entry = nullptr;
            void* ref = nullptr;
            void* slot = nullptr;
            void* player = nullptr;
            if (eng::ReadPtr(eng::At(node, 8), &entry) && entry && eng::ReadPtr(eng::At(entry, kClientEntry_Player), &ref) &&
                ref && eng::ReadPtr(ref, &slot) && slot)
                player = static_cast<char*>(slot) - 8;
            char name[40] = "?";
            uint8_t ai = 0;
            if (player && eng::ReadByte(eng::At(player, eng::kPlayer_IsAI), &ai))
            {
                const void* source = eng::At(player, kPlayer_InlineName);
                void* aiName = nullptr;
                if (ai && eng::ReadPtr(eng::At(player, kPlayer_AiName), &aiName))
                    source = aiName;
                if (!source || !eng::ReadBytes(source, name, sizeof(name) - 1))
                    std::snprintf(name, sizeof(name), "%s", ai ? "(no name)" : "?");
                name[sizeof(name) - 1] = '\0';
                if (!name[0])
                    std::snprintf(name, sizeof(name), "(empty)");
            }
            uint32_t kills = 0, deaths = 0, points = 0, total = 0;
            if (entry)
            {
                eng::ReadU32(eng::At(entry, kClientEntry_Kills), &kills);
                eng::ReadU32(eng::At(entry, kClientEntry_Deaths), &deaths);
                eng::ReadU32(eng::At(entry, kClientEntry_Points), &points);
                eng::ReadU32(eng::At(entry, kClientEntry_TotalPoints), &total);
            }
            ++rows;
            if (used + 72 < sizeof(text))
                used += std::snprintf(text + used, sizeof(text) - used, "%s%s%s %u/%u/%u (%u)", rows > 1 ? ", " : "",
                                      name, ai ? " (AI)" : "", kills, deaths, points, total);
            if (!eng::ReadPtr(node, &node))
                break;
        }
    Log("scoreboard (your game, kills/deaths/points (match total points)): %d rows: %s", rows, text);
}

}  // namespace scoreboard
