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
};

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
                Log("objectives: flag %c taken by %s (was %s's)", g_flags[t.flag].letter, TeamName(now.owner), TeamName(t.last.owner));
            else
                Log("objectives: capture point %p now %s's (was %s's)", t.entity, TeamName(now.owner), TeamName(t.last.owner));
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
            o.owner = f.tracked >= 0 && g_tracked[f.tracked].sampled ? static_cast<int>(g_tracked[f.tracked].last.owner)
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
}

void OnLevel(const std::string& key)
{
    if (key == g_key)
        return;
    g_key = key;
    g_flags.clear();
    for (Tracked& t : g_tracked)
        t.flag = -1;
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

void Tick(const std::vector<brain::Actor>& actors, float dt)
{
    if (!g_installed)
        return;
    g_sampleTimer -= dt;
    if (g_sampleTimer > 0.0f)
        return;
    g_sampleTimer = 0.5f;
    Sync();
    SampleAll(actors);
    Publish();

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
