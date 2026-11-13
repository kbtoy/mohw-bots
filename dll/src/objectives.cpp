#include "objectives.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine.h"
#include "hook.h"
#include "log.h"

namespace objectives {
namespace {

struct FlagData
{
    const char* map;
    char letter;
    float pos[3];
};
const FlagData kSectorControlFlags[] = {
#include "objectives.inc"
};

// ServerCapturePointEntity (README "Game modes"): vtable, owner team, the team whose flag is up, capture progress and
// flag height (both sent to clients / 100, so 0..100 here).
constexpr uintptr_t kCapturePointVtable = 0x0281B450;
constexpr int kCp_Owner = 0xA8;
constexpr int kCp_FlagTeam = 0xAC;
constexpr int kCp_Progress = 0xC0;
constexpr int kCp_Height = 0xC4;

using CtorFn = void*(__thiscall*)(void* self, void* creationInfo, void* data);
using DtorFn = void(__thiscall*)(void* self);
CtorFn g_ctor = nullptr;
DtorFn g_dtor = nullptr;
CRITICAL_SECTION g_lock;
bool g_installed = false;
std::vector<void*> g_live;  // under g_lock: every capture point entity alive now

// ServerPreRoundEntity (0xBC bytes, vtable 0x2822F28; README "Game modes", "The pre-round"). Every mode's round starts
// from it (pf_preround_wrapper's StartRound: Sector Control's tickets and server logic, the round timers). Its message
// listener at +0x90 (vtable 0x2822EDC slot 0, 0xEDEF20, thiscall(msg) ret 4) keeps a list of the players that entered
// the level (ServerPlayerEnteredLevelMessage in, ServerPlayerReleasingLevelMessage out) and, on those and on every
// ServerPlayerChangedSoldierMessage, starts the countdown once RoundStartPlayerCount of them have a soldier and each team
// has half that. The data asks for 8. Bots never enter the level (ServerPlayer +0x504 / +0x505 come from a client
// connection loading it), so offline only you are on the list and the round never started. Offsets from the listener.
constexpr int kPr_StartCount = 0x14;  // entity +0xA4, from PreRoundEntityData.RoundStartPlayerCount
constexpr int kPr_Countdown = 0x1C;   // entity +0xAC, RoundStartCountdown (s)
constexpr int kPr_State = 0x28;       // entity +0xB8: bit 0 the countdown has started
constexpr uint32_t kPreRoundStartPlayers = 1;
using PreRoundListenFn = void(__thiscall*)(void* self, void* msg);
PreRoundListenFn g_preRoundListen = nullptr;
void* g_preRoundSeen = nullptr;  // server thread: the listener last seen (one pre-round per level)
uint8_t g_preRoundState = 0;

void __fastcall PreRoundListenDetour(void* self, void* /*edx*/, void* msg)
{
    if (self != g_preRoundSeen)
    {
        g_preRoundSeen = self;
        g_preRoundState = 0;
    }
    uint32_t need = 0;
    void* count = const_cast<void*>(eng::At(self, kPr_StartCount));
    if (eng::ReadU32(count, &need) && need > kPreRoundStartPlayers && eng::WriteBytes(count, &kPreRoundStartPlayers, 4))
        Log("pre-round: the level wants %u players with a soldier to start the round; bots never count, so it now starts "
            "with %u (you, once spawned)", need, kPreRoundStartPlayers);
    g_preRoundListen(self, msg);
    uint8_t state = 0;
    if (eng::ReadByte(eng::At(self, kPr_State), &state))
    {
        if ((state & 1) && !(g_preRoundState & 1))
        {
            float countdown = 0.0f;
            eng::ReadFloat(eng::At(self, kPr_Countdown), &countdown);
            Log("pre-round: enough players; the round starts in %.0f s", countdown);
        }
        g_preRoundState = state;
    }
}

// 2026.11.11.1 lowered the count and nothing changed: no "pre-round:" line at all, so the listener never ran. Whether a
// ServerPreRoundEntity exists at all is the next question: its constructor (0xEDED10, thiscall(creationInfo, data) ret 8)
// is hooked to list them, and Tick logs each one's state as it changes (and lowers the count there too).
constexpr uintptr_t kPreRoundVtable = 0x02822F28;
constexpr int kPre_ListBegin = 0x94, kPre_ListEnd = 0x98, kPre_StartCount = 0xA4, kPre_Countdown = 0xAC, kPre_State = 0xB8;
using PreRoundCtorFn = void*(__thiscall*)(void* self, void* creationInfo, void* data);
PreRoundCtorFn g_preRoundCtor = nullptr;
struct PreRound
{
    void* entity = nullptr;
    ULONGLONG createdAt = 0;
    bool logged = false;
    uint32_t count = 0, listed = 0;
    uint8_t state = 0;
};
std::vector<PreRound> g_preRounds;  // under g_lock

void* __fastcall PreRoundCtorDetour(void* self, void* /*edx*/, void* creationInfo, void* data)
{
    void* result = g_preRoundCtor(self, creationInfo, data);
    PreRound p;
    p.entity = self;
    p.createdAt = GetTickCount64();
    EnterCriticalSection(&g_lock);
    g_preRounds.push_back(p);
    LeaveCriticalSection(&g_lock);
    Log("pre-round: entity %p created (data %p)", self, data);
    return result;
}

void WatchPreRounds()
{
    EnterCriticalSection(&g_lock);
    for (PreRound& p : g_preRounds)
    {
        void* vtable = nullptr;
        if (!eng::ReadPtr(p.entity, &vtable) || reinterpret_cast<uintptr_t>(vtable) != kPreRoundVtable)
            continue;
        uint32_t count = 0, begin = 0, end = 0;
        uint8_t state = 0;
        float countdown = 0.0f;
        if (!eng::ReadU32(eng::At(p.entity, kPre_StartCount), &count) || !eng::ReadU32(eng::At(p.entity, kPre_ListBegin), &begin) ||
            !eng::ReadU32(eng::At(p.entity, kPre_ListEnd), &end) || !eng::ReadByte(eng::At(p.entity, kPre_State), &state))
            continue;
        eng::ReadFloat(eng::At(p.entity, kPre_Countdown), &countdown);
        const uint32_t listed = end >= begin ? (end - begin) / 4 : 0;
        if (!p.logged || count != p.count || listed != p.listed || state != p.state)
        {
            Log("pre-round: entity %p wants %u player(s), has %u entered, state 0x%02X (bit 0 = counting down), countdown %.0f s",
                p.entity, count, listed, state, countdown);
            p.logged = true;
            p.count = count;
            p.listed = listed;
            p.state = state;
        }
        if (count > kPreRoundStartPlayers)
            eng::WriteBytes(const_cast<void*>(eng::At(p.entity, kPre_StartCount)), &kPreRoundStartPlayers, 4);
    }
    LeaveCriticalSection(&g_lock);
}

struct Sample
{
    uint32_t owner = 0;
    uint32_t flagTeam = 0;
    float progress = 0.0f;
    float height = 0.0f;
};

struct Tracked
{
    void* entity = nullptr;
    int flag = -1;  // index into g_flags once learned
    Sample last;
    bool sampled = false;
    Sample logged;          // what the last capture line said
    float logTimer = 0.0f;  // seconds until another capture line may go out
    // The whole entity as last logged (2026-11-12: nothing moved with you standing on a flag, so find out which of
    // its fields, if any, react; unknown ones included).
    static constexpr int kRawWords = 0x104 / 4;
    uint32_t raw[kRawWords] = {};
    bool rawValid = false;
    float rawTimer = 0.0f;
};
int g_rawLines = 0;  // per level, capped

// Who holds a flag: the team in control (+0xA8) once its flag is the one up (+0xAC, set at the top of a capture); mid
// capture (control taken at the bottom, flag still being raised) nobody yet.
int Held(const Tracked& t)
{
    return t.sampled && t.last.owner == t.last.flagTeam ? static_cast<int>(t.last.owner) : 0;
}

struct Flag
{
    char letter = '?';
    float pos[3] = {};
    int tracked = -1;  // index into g_tracked once learned
};

std::vector<Tracked> g_tracked;  // server thread
std::vector<Flag> g_flags;
std::string g_key;
bool g_flagMode = false;
float g_sampleTimer = 0.0f;
float g_summaryTimer = 0.0f;
size_t g_reportedCount = 0;
float g_countTimer = 0.0f;

void* __fastcall CtorDetour(void* self, void* /*edx*/, void* creationInfo, void* data)
{
    void* result = g_ctor(self, creationInfo, data);
    EnterCriticalSection(&g_lock);
    g_live.push_back(self);
    LeaveCriticalSection(&g_lock);
    return result;
}

void __fastcall DtorDetour(void* self, void* /*edx*/)
{
    EnterCriticalSection(&g_lock);
    g_live.erase(std::remove(g_live.begin(), g_live.end(), self), g_live.end());
    LeaveCriticalSection(&g_lock);
    g_dtor(self);
}

bool Read(void* entity, Sample* out)
{
    void* vtable = nullptr;
    return eng::ReadPtr(entity, &vtable) && reinterpret_cast<uintptr_t>(vtable) == kCapturePointVtable &&
           eng::ReadU32(eng::At(entity, kCp_Owner), &out->owner) && eng::ReadU32(eng::At(entity, kCp_FlagTeam), &out->flagTeam) &&
           eng::ReadFloat(eng::At(entity, kCp_Progress), &out->progress) && eng::ReadFloat(eng::At(entity, kCp_Height), &out->height);
}

const char* TeamName(uint32_t team)
{
    static const char* names[] = {"nobody", "team 1", "team 2"};
    return team <= 2 ? names[team] : "team ?";
}

float Flat(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

// Soldiers standing at a flag (inside its capture area, give or take).
int SoldiersAt(const Flag& flag, const std::vector<brain::Actor>& actors, int counts[3])
{
    counts[0] = counts[1] = counts[2] = 0;
    int n = 0;
    for (const brain::Actor& a : actors)
        if (Flat(a.pos, flag.pos) < 8.0f && std::fabs(a.pos[1] - flag.pos[1]) < 5.0f)
        {
            ++n;
            if (a.team >= 0 && a.team <= 2)
                ++counts[a.team];
        }
    return n;
}

// Keeps g_tracked in step with the live entities (new ones added, destroyed ones dropped, flags re-linked).
void Sync()
{
    std::vector<void*> live;
    EnterCriticalSection(&g_lock);
    live = g_live;
    LeaveCriticalSection(&g_lock);
    bool changed = false;
    for (size_t i = g_tracked.size(); i-- > 0;)
        if (std::find(live.begin(), live.end(), g_tracked[i].entity) == live.end())
        {
            g_tracked.erase(g_tracked.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
        }
    for (void* e : live)
        if (std::none_of(g_tracked.begin(), g_tracked.end(), [e](const Tracked& t) { return t.entity == e; }))
        {
            Tracked t;
            t.entity = e;
            g_tracked.push_back(t);
            changed = true;
        }
    if (!changed)
        return;
    for (Flag& f : g_flags)
        f.tracked = -1;
    for (size_t i = 0; i < g_tracked.size(); ++i)
        if (g_tracked[i].flag >= 0 && g_tracked[i].flag < static_cast<int>(g_flags.size()))
            g_flags[g_tracked[i].flag].tracked = static_cast<int>(i);
}

void Link(int tracked, int flag, const char* how)
{
    g_tracked[tracked].flag = flag;
    g_flags[flag].tracked = tracked;
    Log("objectives: flag %c is capture point %p (%s)", g_flags[flag].letter, g_tracked[tracked].entity, how);
}

// Which flag an entity is, from its data: CapturePointEntityData.FlagID (+0xE0; SC_FlagA 0, B 1, C 2, SpawnFlag 3) -
// the A / B / C control point prefabs say 0 / 1 / 2 on every map, the same prefabs objectives.py reads the places from.
constexpr int kCp_Data = 0x14;
constexpr int kData_CaptureRadius = 0x8C, kData_FlagId = 0xE0;

void LinkByFlagId()
{
    for (size_t i = 0; i < g_tracked.size(); ++i)
    {
        if (g_tracked[i].flag >= 0)
            continue;
        void* data = nullptr;
        uint32_t id = 0;
        if (!eng::ReadPtr(eng::At(g_tracked[i].entity, kCp_Data), &data) || !data ||
            !eng::ReadU32(eng::At(data, kData_FlagId), &id) || id > 2)
            continue;
        for (size_t f = 0; f < g_flags.size(); ++f)
            if (g_flags[f].letter == static_cast<char>('A' + id) && g_flags[f].tracked < 0)
            {
                char how[48];
                std::snprintf(how, sizeof(how), "its data's FlagID is %u", id);
                Link(static_cast<int>(i), static_cast<int>(f), how);
                break;
            }
    }
}

// The capture itself (2026-11-12: nothing ever moved, for anyone). ServerCapturePointEntity's update slot (+0x88) is a
// bare ret 4 in this build and nothing calls its capture step (0xE47F30), so this does what the missing caller would:
// every 0.5 s, for each flag, count the living soldiers within its CaptureRadius (5 m flat, 3 m up or down) by the
// controlling team (+0xA8): [0] the others (attackers), [1] the controller's; no squad counts (no squad bonus). With
// nobody in control, the bigger team present attacks and the other counts as holding. The game does the rest: rates
// (MinNrToTakeControl, MaxCaptureMultiplier, AddedMultiplierPerPlayer, seesaw), the flag lowering then raising, setTeam,
// OnCaptured, the Captured / Lost messages (tickets, spawns, HUD) and capture points for those inside.
struct CaptureCounts
{
    int32_t attackers = 0, holders = 0;
    int32_t squads[14] = {};
};
struct PlayerList
{
    void** begin = nullptr;
    void** end = nullptr;
};
struct FlagCrowd
{
    int team1 = -1, team2 = -1;  // last logged, -1 never
    int lines = 0;
};
FlagCrowd g_crowd[3];
bool g_captureFailed = false;

void DriveCaptures(const std::vector<brain::Actor>& actors, float dt)
{
    if (!g_flagMode || g_captureFailed)
        return;
    for (Tracked& t : g_tracked)
    {
        if (t.flag < 0 || t.flag >= static_cast<int>(g_flags.size()))
            continue;
        const Flag& flag = g_flags[t.flag];
        void* data = nullptr;
        float radius = 5.0f;
        uint32_t controller = 0;
        if (!eng::ReadPtr(eng::At(t.entity, kCp_Data), &data) || !data || !eng::ReadFloat(eng::At(data, kData_CaptureRadius), &radius) ||
            !eng::ReadU32(eng::At(t.entity, kCp_Owner), &controller) || radius <= 0.0f || radius > 50.0f)
            continue;
        void* inside[64];
        int n = 0, byTeam[3] = {};
        for (const brain::Actor& a : actors)
            if ((a.team == 1 || a.team == 2) && a.player && Flat(a.pos, flag.pos) < radius && std::fabs(a.pos[1] - flag.pos[1]) < 3.0f)
            {
                ++byTeam[a.team];
                if (n < 64)
                    inside[n++] = a.player;
            }
        CaptureCounts counts;
        int attacker = 0;
        if (controller == 1 || controller == 2)
        {
            attacker = 3 - static_cast<int>(controller);
            counts.attackers = byTeam[attacker];
            counts.holders = byTeam[controller];
        }
        else
        {
            attacker = byTeam[1] >= byTeam[2] ? 1 : 2;
            counts.attackers = byTeam[attacker];
            counts.holders = byTeam[3 - attacker];
        }
        PlayerList list;
        list.begin = inside;
        list.end = inside + n;
        if (!eng::CapturePointUpdate(t.entity, dt, &list, &counts, attacker))
        {
            g_captureFailed = true;
            Log("objectives: the capture step FAILED on flag %c; flags won't capture for the rest of this session", flag.letter);
            return;
        }
        FlagCrowd& crowd = g_crowd[std::min(t.flag, 2)];
        if ((byTeam[1] != crowd.team1 || byTeam[2] != crowd.team2) && crowd.lines < 200)
        {
            ++crowd.lines;
            crowd.team1 = byTeam[1];
            crowd.team2 = byTeam[2];
            Log("objectives: flag %c: %d of team 1 and %d of team 2 inside (%s in control)", flag.letter, byTeam[1], byTeam[2],
                TeamName(controller));
        }
    }
}

void SampleAll(const std::vector<brain::Actor>& actors)
{
    for (size_t i = 0; i < g_tracked.size(); ++i)
    {
        Tracked& t = g_tracked[i];
        Sample now;
        if (!Read(t.entity, &now))
            continue;
        const bool moved = t.sampled && (now.owner != t.last.owner || now.flagTeam != t.last.flagTeam ||
                                         std::fabs(now.progress - t.last.progress) > 0.01f ||
                                         std::fabs(now.height - t.last.height) > 0.01f);
        if (t.sampled && now.owner != t.last.owner)
        {
            if (t.flag >= 0)
                Log("objectives: flag %c: %s in control (was %s)", g_flags[t.flag].letter, TeamName(now.owner), TeamName(t.last.owner));
            else
                Log("objectives: capture point %p now %s's (was %s's)", t.entity, TeamName(now.owner), TeamName(t.last.owner));
        }
        if (t.sampled && t.flag >= 0)
        {
            Tracked next = t;
            next.last = now;
            if (Held(next) != Held(t))
                Log("objectives: flag %c %s", g_flags[t.flag].letter,
                    Held(next) ? (Held(next) == 1 ? "captured by team 1" : "captured by team 2") : "lost: nobody holds it");
        }
        // A capture under way: progress or the flag moving, at most every 2 s per point (2026-11-11: no capture was ever
        // seen, yours included, so show what the entities do).
        t.logTimer -= 0.5f;
        if (t.sampled && t.logTimer <= 0.0f &&
            (std::fabs(now.progress - t.logged.progress) >= 5.0f || std::fabs(now.height - t.logged.height) >= 5.0f ||
             now.flagTeam != t.logged.flagTeam))
        {
            char name[32];
            if (t.flag >= 0)
                std::snprintf(name, sizeof(name), "flag %c", g_flags[t.flag].letter);
            else
                std::snprintf(name, sizeof(name), "capture point %p", t.entity);
            Log("objectives: %s capture %.0f/100, flag at %.0f/100 (%s's flag up, owner %s)", name, now.progress, now.height,
                TeamName(now.flagTeam), TeamName(now.owner));
            t.logged = now;
            t.logTimer = 2.0f;
        }
        if (!t.sampled)
            t.logged = now;
        // Raw: the first read whole, then which words changed (at most every 3 s per point, 150 lines a level).
        uint32_t raw[Tracked::kRawWords];
        t.rawTimer -= 0.5f;
        if (g_rawLines < 150 && eng::ReadBytes(t.entity, raw, sizeof(raw)))
        {
            char text[700] = "";
            int changed = 0;
            for (int w = 0; w < Tracked::kRawWords; ++w)
                if (!t.rawValid || raw[w] != t.raw[w])
                {
                    if (t.rawValid && t.rawTimer > 0.0f)
                        break;  // not now; compared again next time
                    char one[40];
                    if (t.rawValid)
                        std::snprintf(one, sizeof(one), " +%02X %08X>%08X", w * 4, t.raw[w], raw[w]);
                    else
                        std::snprintf(one, sizeof(one), "%s%08X", w % 8 ? " " : (w ? " | " : ""), raw[w]);
                    if (std::strlen(text) + std::strlen(one) < sizeof(text) - 1)
                        std::strcat(text, one);
                    ++changed;
                }
            if (changed && (!t.rawValid || t.rawTimer <= 0.0f))
            {
                ++g_rawLines;
                Log(t.rawValid ? "objectives: capture point %p raw change:%s" : "objectives: capture point %p raw: %s", t.entity, text);
                std::memcpy(t.raw, raw, sizeof(raw));
                t.rawValid = true;
                t.rawTimer = 3.0f;
            }
        }
        // Learn which flag this is: the one flag (not yet linked) with soldiers at it while its progress moves.
        if (moved && t.flag < 0 && g_flagMode)
        {
            int candidate = -1, candidates = 0;
            int counts[3] = {};
            for (size_t f = 0; f < g_flags.size(); ++f)
            {
                int c[3];
                if (g_flags[f].tracked < 0 && SoldiersAt(g_flags[f], actors, c) > 0)
                {
                    candidate = static_cast<int>(f);
                    ++candidates;
                    std::memcpy(counts, c, sizeof(counts));
                }
            }
            if (candidates == 1)
            {
                char how[96];
                std::snprintf(how, sizeof(how), "its capture moved with %d of team 1 and %d of team 2 at it", counts[1], counts[2]);
                Link(static_cast<int>(i), candidate, how);
            }
        }
        t.last = now;
        t.sampled = true;
    }
    // The last flag and the last capture point left: each other's.
    if (g_flagMode && g_tracked.size() == g_flags.size())
    {
        int freeTracked = -1, freeFlag = -1, nTracked = 0, nFlags = 0;
        for (size_t i = 0; i < g_tracked.size(); ++i)
            if (g_tracked[i].flag < 0)
            {
                freeTracked = static_cast<int>(i);
                ++nTracked;
            }
        for (size_t f = 0; f < g_flags.size(); ++f)
            if (g_flags[f].tracked < 0)
            {
                freeFlag = static_cast<int>(f);
                ++nFlags;
            }
        if (nTracked == 1 && nFlags == 1)
            Link(freeTracked, freeFlag, "the only ones left");
    }
}

// The bots' view: each flag with its owner, or -1 while it isn't linked and the unlinked entities disagree.
void Publish()
{
    std::vector<brain::Objective> list;
    if (g_flagMode)
    {
        int unlinkedOwner = -2;  // -2 none seen, -1 they differ
        for (const Tracked& t : g_tracked)
            if (t.flag < 0 && t.sampled)
                unlinkedOwner = unlinkedOwner == -2 ? static_cast<int>(t.last.owner)
                                : unlinkedOwner == static_cast<int>(t.last.owner) ? unlinkedOwner : -1;
        for (const Flag& f : g_flags)
        {
            brain::Objective o;
            std::memcpy(o.pos, f.pos, sizeof(o.pos));
            o.letter = f.letter;
            o.radius = 4.0f;
            o.owner = f.tracked >= 0 && g_tracked[f.tracked].sampled ? Held(g_tracked[f.tracked])
                      : unlinkedOwner >= 0 ? unlinkedOwner : -1;
            list.push_back(o);
        }
    }
    brain::SetObjectives(list);
}

}  // namespace

void Install()
{
    InitializeCriticalSection(&g_lock);
    void* ctor = reinterpret_cast<void*>(eng::Address("CapturePoint_ctor"));
    void* dtor = reinterpret_cast<void*>(eng::Address("CapturePoint_dtor"));
    if (ctor && dtor)
    {
        g_ctor = reinterpret_cast<CtorFn>(InlineHook(ctor, 7, reinterpret_cast<const void*>(&CtorDetour)));
        g_dtor = g_ctor ? reinterpret_cast<DtorFn>(InlineHook(dtor, 6, reinterpret_cast<const void*>(&DtorDetour))) : nullptr;
    }
    g_installed = g_ctor && g_dtor;
    Log("objectives: capture point hooks %s", g_installed ? "in place" : "FAILED (bots won't play Sector Control's flags)");

    // The pre-round listener: mov eax, [esp+4]; cmp dword ptr [eax+4], 0xF3C93DD3 (4 + 7 position-independent bytes).
    void* listen = reinterpret_cast<void*>(eng::Address("PreRound_listen"));
    if (listen)
        g_preRoundListen = reinterpret_cast<PreRoundListenFn>(InlineHook(listen, 11, reinterpret_cast<const void*>(&PreRoundListenDetour)));
    Log("objectives: pre-round hook %s", g_preRoundListen ? "in place (a round starts once you have spawned)"
                                                          : "FAILED (rounds never start: no Sector Control tickets or flags)");
    // Its constructor: mov eax, [esp+8]; push esi (4 + 1 position-independent bytes).
    void* preCtor = reinterpret_cast<void*>(eng::Address("PreRound_ctor"));
    if (preCtor)
        g_preRoundCtor = reinterpret_cast<PreRoundCtorFn>(InlineHook(preCtor, 5, reinterpret_cast<const void*>(&PreRoundCtorDetour)));
    Log("objectives: pre-round constructor hook %s", g_preRoundCtor ? "in place" : "FAILED");
}

void OnLevel(const std::string& key)
{
    if (key == g_key)
        return;
    g_key = key;
    g_flags.clear();
    for (Tracked& t : g_tracked)
        t.flag = -1;
    g_rawLines = 0;
    for (FlagCrowd& c : g_crowd)
        c = FlagCrowd();
    // The last level's pre-round is gone; keep one made in the last 60 s (this level's, if it came first).
    EnterCriticalSection(&g_lock);
    const ULONGLONG now = GetTickCount64();
    g_preRounds.erase(std::remove_if(g_preRounds.begin(), g_preRounds.end(),
                                     [now](const PreRound& p) { return now - p.createdAt > 60000; }),
                      g_preRounds.end());
    LeaveCriticalSection(&g_lock);
    g_flagMode = false;
    g_reportedCount = 0;
    g_countTimer = 0.0f;
    const std::string mode = key.size() > 6 ? key.substr(6) : std::string();
    if (mode == "SectorControl")
    {
        for (const FlagData& d : kSectorControlFlags)
            if (key.compare(0, 5, d.map) == 0)
            {
                Flag f;
                f.letter = d.letter;
                std::memcpy(f.pos, d.pos, sizeof(f.pos));
                g_flags.push_back(f);
            }
        g_flagMode = g_installed && !g_flags.empty();
        if (g_flags.empty())
            Log("objectives: no Sector Control flag positions for %s; bots play it as Team Deathmatch", key.c_str());
        else
        {
            char text[160] = "";
            for (const Flag& f : g_flags)
            {
                char one[48];
                std::snprintf(one, sizeof(one), "%s%c (%.0f, %.0f, %.0f)", text[0] ? ", " : "", f.letter, f.pos[0], f.pos[1], f.pos[2]);
                std::strncat(text, one, sizeof(text) - std::strlen(text) - 1);
            }
            Log("objectives: Sector Control flags %s; bots go for the ones their team doesn't hold%s", text,
                g_installed ? "" : " - NOT, the hooks failed");
        }
    }
    Publish();
}

bool HasFlags() { return g_flagMode && !g_flags.empty(); }

bool NearestFlag(const float p[3], char* letter, float pos[3], int* owner)
{
    if (!g_flagMode || g_flags.empty())
        return false;
    size_t best = 0;
    for (size_t f = 1; f < g_flags.size(); ++f)
        if (Flat(p, g_flags[f].pos) < Flat(p, g_flags[best].pos))
            best = f;
    const Flag& f = g_flags[best];
    if (letter)
        *letter = f.letter;
    if (pos)
        std::memcpy(pos, f.pos, sizeof(f.pos));
    if (owner)
        *owner = f.tracked >= 0 ? Held(g_tracked[f.tracked]) : -1;
    return true;
}

void Tick(const std::vector<brain::Actor>& actors, float dt)
{
    if (!g_installed)
        return;
    g_sampleTimer -= dt;
    if (g_sampleTimer > 0.0f)
        return;
    g_sampleTimer = 0.5f;
    Sync();
    if (g_flagMode)
        LinkByFlagId();
    DriveCaptures(actors, 0.5f);
    SampleAll(actors);
    Publish();
    WatchPreRounds();

    // What the level has, once it settles (any mode: the next modes' objectives start from this).
    g_countTimer += 0.5f;
    if (g_countTimer >= 20.0f && g_tracked.size() != g_reportedCount)
    {
        g_reportedCount = g_tracked.size();
        char text[256] = "";
        for (const Tracked& t : g_tracked)
        {
            char one[96];
            if (t.sampled)
                std::snprintf(one, sizeof(one), "%s%p owner %s, %s's flag up, capture %.0f, flag at %.0f", text[0] ? "; " : "",
                              t.entity, TeamName(t.last.owner), TeamName(t.last.flagTeam), t.last.progress, t.last.height);
            else
                std::snprintf(one, sizeof(one), "%s%p unreadable", text[0] ? "; " : "", t.entity);
            std::strncat(text, one, sizeof(text) - std::strlen(text) - 1);
        }
        Log("objectives: %u capture point entities in %s%s%s", static_cast<unsigned>(g_tracked.size()), g_key.c_str(),
            text[0] ? ": " : "", text);
    }
    g_summaryTimer += 0.5f;
    if (g_flagMode && g_summaryTimer >= 60.0f)
    {
        g_summaryTimer = 0.0f;
        char text[160] = "";
        for (const Flag& f : g_flags)
        {
            char one[48];
            const char* owner = f.tracked >= 0 ? TeamName(g_tracked[f.tracked].last.owner) : "not linked yet";
            std::snprintf(one, sizeof(one), "%s%c %s", text[0] ? ", " : "", f.letter, owner);
            std::strncat(text, one, sizeof(text) - std::strlen(text) - 1);
        }
        Log("objectives (last minute): %s", text);
    }
}

}  // namespace objectives
