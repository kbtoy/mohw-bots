#include "combatlog.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <unordered_map>

#include "engine.h"
#include "hook.h"
#include "log.h"

namespace combatlog {
namespace {

// ServerWeaponPlayerPrimaryFireShotSpawnedMessage: +0x28 shooter player.
constexpr uint32_t kCategoryWeapon = 0x07038862;
constexpr uint32_t kShotSpawned = 0xD3A8C934;
// ServerSoldierSoldierDamageMessage (sent by 0xE39170, from 0xE3CBB0, for damage with a known giver player): +0x28
// victim soldier, +0x2C DamageGiverInfo (-> [[info]] - 8 = the giver's player), +0x30 health taken (before - new,
// clamped), +0x34 health before, +0x38 raw damage.
constexpr uint32_t kCategoryServerSoldier = 0xDC84F4EA;
constexpr uint32_t kSoldierDamage = 0x9178C980;
// DamageArbitrationClientDamageMessage (a client's hit claim, handled by 0xE41590 when
// SyncedGameSettings.AllowClientSideDamageArbitration is set): +0x28 victim player id, +0x30 giver player id,
// +0x50 damage claimed.
constexpr uint32_t kCategoryArbitration = 0x26BB582D;
constexpr uint32_t kClientDamageClaim = 0x46E163B4;
constexpr uintptr_t kTypeInfo_SyncedGameSettings = 0x2B7447C;  // +0x1E AllowClientSideDamageArbitration
// ServerCollisionProjectileFireMessage (sent by 0xBC63CE as a projectile is created, one per bullet): +0x28 the
// projectile entity, +0x30 its spawn position. The projectile's velocity is at entity +0x120 (its collision handler
// 0xBD0200 reads it there).
constexpr uint32_t kCategoryCollision = 0xDB60E01E;
constexpr uint32_t kProjectileFire = 0xAF088758;
constexpr int kProjectile_Velocity = 0x120;

struct Event
{
    bool shot = false;
    bool claim = false;
    bool bullet = false;  // a projectile was fired: pos, vel
    float pos[3] = {};
    float vel[3] = {};
    int giverId = -1;
    int victimId = -1;
    float claimed = 0.0f;
    void* giver = nullptr;   // player
    void* victim = nullptr;  // soldier
    float before = 0.0f;
    float after = 0.0f;
};

struct Stats
{
    int shots = 0;
    int hits = 0;
    int zeroHits = 0;  // damage events that took no health
    float damage = 0.0f;
    int hitsOnYou = 0;
    float damageOnYou = 0.0f;
    int kills = 0;
    int headshotKills = 0;
};

// Your hits on one bot, merged while they keep coming (a shotgun blast is several pellets).
struct Burst
{
    int claims = 0;
    float claimed = 0.0f;
    int hits = 0;
    int zeroHits = 0;
    float before = 0.0f;
    float after = 0.0f;
    DWORD last = 0;
};

bool g_enabled = true;
bool g_logYourHits = true;
CRITICAL_SECTION g_lock;
std::vector<Event> g_events;
std::unordered_map<void*, Stats> g_stats;      // player -> this minute's numbers
std::unordered_map<void*, void*> g_ownerOf;     // soldier -> player (kept after the soldier dies)
std::unordered_map<void*, DWORD> g_ownerSeen;
std::unordered_map<void*, Burst> g_bursts;      // victim player -> your current burst
DWORD g_windowStart = 0;
int g_hitLines = 0;
int g_onYouLines = 0;

// What reaches the server's soldier damage entry (0xB64FE0) - every bullet / explosion that collided with a soldier
// - before it decides to apply or drop it.
struct DamageCall
{
    void* soldier = nullptr;  // the victim
    void* giver = nullptr;    // the shooter's player ([[giverInfo]] - 8), null if none
    bool arbitrable = false;  // damageInfo +0x6D bit 8
    uint8_t raw[0x80] = {};   // the damageInfo, for the first few samples
};
std::vector<DamageCall> g_damageCalls;
using DamageFn = void(__thiscall*)(void* soldier, void* info, void* giverInfo, void* a3);
DamageFn g_damage = nullptr;
using ClaimFn = void(__thiscall*)(void* self, void* message);
ClaimFn g_claim = nullptr;
bool g_serverBotHits = true;            // [bots] BotHitsOnBotsByServer
std::unordered_map<void*, bool> g_botSoldiers;  // soldier -> is a bot's (server thread; rebuilt every tick)
int g_appliedBotHits = 0, g_ignoredBotClaims = 0, g_applyFaults = 0;

constexpr uintptr_t kServerGameContextPtr = 0x2AEAF8C;
constexpr int kPlayer_IsAI = 0x354;

bool IsAIPlayer(void* player)
{
    uint8_t ai = 0;
    return player && eng::ReadByte(eng::At(player, kPlayer_IsAI), &ai) && ai != 0;
}

// ServerPlayerManager (ServerGameContext +0x30) +0xA4: players by id (< 0x80), as the claim handler looks them up.
void* PlayerById(uint32_t id)
{
    void* context = nullptr;
    void* manager = nullptr;
    void* table = nullptr;
    void* player = nullptr;
    if (id >= 0x80 || !eng::ReadPtr(reinterpret_cast<const void*>(kServerGameContextPtr), &context) || !context ||
        !eng::ReadPtr(eng::At(context, 0x30), &manager) || !manager || !eng::ReadPtr(eng::At(manager, 0xA4), &table) ||
        !table || !eng::ReadPtr(eng::At(table, static_cast<int>(id * 4)), &player))
        return nullptr;
    return player;
}

// Applies a hit as the damage entry does for an unflagged one: the soldier's damage handler (+0x6E8) vfunc 0
// (damageInfo, giverInfo). False on a fault (then the game's own path runs).
bool ApplyHit(void* soldier, void* info, void* giverInfo)
{
    __try
    {
        void* handler = *reinterpret_cast<void**>(static_cast<char*>(soldier) + 0x6E8);
        if (!handler)
            return false;
        using ApplyFn = void(__thiscall*)(void* self, void* info, void* giverInfo);
        reinterpret_cast<ApplyFn>((*reinterpret_cast<void***>(handler))[0])(handler, info, giverInfo);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Bot -> bot hits seen from both sides, to tell which of your game's claims the server never matched (server thread).
// Your game applies its own claims at once; the server ignores them (it applies its own bot -> bot hits). A claim the
// server matched - a hit it applied from the same shooter on the same soldier within 0.3 s, before or after - changes
// nothing. One it never matched is damage only your game dealt: when such damage, this claim included, reaches the
// victim's health on the server, your game shows the bot dead (ragdoll) while the server's hits haven't killed it - it
// went on fighting as a limp body. That claim is applied as a killing blow (damage raised past the server's health),
// so the bot dies on both sides. Client-only damage counts for 3 s.
struct SeenHit
{
    void* soldier = nullptr;
    void* giver = nullptr;
    DWORD tick = 0;
    float damage = 0.0f;
};
std::vector<SeenHit> g_serverHits;   // applied by the server, not matched to a claim yet
std::vector<SeenHit> g_pendingClaims;  // ignored claims not matched to a server hit yet
std::vector<SeenHit> g_clientOnly;   // claims the server never matched
constexpr DWORD kMatchMs = 300;
constexpr DWORD kClientOnlyMs = 3000;
int g_lethalBotClaims = 0, g_unmatchedClaims = 0;

// Drops what's too old to match; claims left unmatched become client-only damage.
void AgeSeenHits(DWORD now)
{
    auto old = [now](DWORD limit) { return [now, limit](const SeenHit& h) { return now - h.tick > limit; }; };
    g_serverHits.erase(std::remove_if(g_serverHits.begin(), g_serverHits.end(), old(kMatchMs)), g_serverHits.end());
    for (const SeenHit& c : g_pendingClaims)
        if (now - c.tick > kMatchMs)
        {
            g_clientOnly.push_back(c);
            ++g_unmatchedClaims;
        }
    g_pendingClaims.erase(std::remove_if(g_pendingClaims.begin(), g_pendingClaims.end(), old(kMatchMs)),
                          g_pendingClaims.end());
    g_clientOnly.erase(std::remove_if(g_clientOnly.begin(), g_clientOnly.end(), old(kClientOnlyMs)), g_clientOnly.end());
}

// Takes the first entry for this soldier and shooter out of `list`; false if there is none.
bool TakeMatch(std::vector<SeenHit>& list, void* soldier, void* giver)
{
    for (auto it = list.begin(); it != list.end(); ++it)
        if (it->soldier == soldier && it->giver == giver)
        {
            list.erase(it);
            return true;
        }
    return false;
}

// A server-applied bot -> bot hit (DamageDetour).
void OnServerBotHit(void* soldier, void* giver)
{
    DWORD now = GetTickCount();
    AgeSeenHits(now);
    if (!TakeMatch(g_pendingClaims, soldier, giver))
        g_serverHits.push_back({soldier, giver, now, 0.0f});
}

// A client's hit claim (DamageArbitrationClientDamageMessage) where a bot hit a bot: see SeenHit above.
void __fastcall ClaimDetour(void* self, void* /*edx*/, void* message)
{
    if (g_serverBotHits && message)
    {
        uint32_t type = 0, victimId = 0, giverId = 0;
        void* victim = nullptr;
        void* giver = nullptr;
        if (eng::ReadU32(eng::At(message, 8), &type) && type == kClientDamageClaim &&
            eng::ReadU32(eng::At(message, 0x28), &victimId) && eng::ReadU32(eng::At(message, 0x30), &giverId) &&
            IsAIPlayer(giver = PlayerById(giverId)) && IsAIPlayer(victim = PlayerById(victimId)))
        {
            DWORD now = GetTickCount();
            AgeSeenHits(now);
            void* soldier = nullptr;
            float claimed = 0.0f, health = 0.0f, clientOnly = 0.0f;
            eng::ReadFloat(eng::At(message, 0x50), &claimed);
            eng::ReadPtr(eng::At(victim, 0x42C), &soldier);
            if (soldier && !TakeMatch(g_serverHits, soldier, giver))
            {
                for (const SeenHit& h : g_clientOnly)
                    clientOnly += h.soldier == soldier ? h.damage : 0.0f;
                if (eng::GetSoldierHealth(soldier, &health) && health > 0.0f && claimed > 0.0f &&
                    clientOnly + claimed >= health)
                {
                    float lethal = health + 100.0f;  // past any armour bonus
                    if (claimed < lethal)
                        *reinterpret_cast<float*>(static_cast<char*>(message) + 0x50) = lethal;
                    g_clientOnly.erase(std::remove_if(g_clientOnly.begin(), g_clientOnly.end(),
                                                      [soldier](const SeenHit& h) { return h.soldier == soldier; }),
                                       g_clientOnly.end());
                    ++g_lethalBotClaims;
                    g_claim(self, message);
                    return;
                }
                g_pendingClaims.push_back({soldier, giver, now, claimed});
            }
            ++g_ignoredBotClaims;
            return;
        }
    }
    g_claim(self, message);
}
struct DamageCounts
{
    int calls[2][3] = {};       // [victim: bot, you][shooter: bot, you, none]
    int arbitrable[2][3] = {};  // of those, flagged client-arbitrable
};
DamageCounts g_damageCounts;
int g_damageSamples[2] = {};  // raw dumps logged: bot -> bot, bot -> you

void __fastcall DamageDetour(void* soldier, void* /*edx*/, void* info, void* giverInfo, void* a3)
{
    DamageCall c;
    c.soldier = soldier;
    void* handle = nullptr;
    void* inner = nullptr;
    if (giverInfo && eng::ProbePtr(giverInfo, &handle) && handle && eng::ProbePtr(handle, &inner) && inner)
        c.giver = static_cast<char*>(inner) - 8;
    uint8_t flags = 0;
    if (info && eng::ProbeByte(eng::At(info, 0x6D), &flags))
        c.arbitrable = (flags & 8) != 0;
    if (info && g_enabled)
        eng::ReadBytes(info, c.raw, sizeof(c.raw));
    if (g_enabled)
    {
        EnterCriticalSection(&g_lock);
        if (g_damageCalls.size() < 4096)
            g_damageCalls.push_back(c);
        LeaveCriticalSection(&g_lock);
    }
    // A bot's hit on a bot, flagged client-arbitrable: the game would drop it (AI shooter) and wait for a player's
    // report that only comes when your game happens to see it. Apply it here instead.
    if (g_serverBotHits && c.arbitrable && IsAIPlayer(c.giver))
    {
        auto bot = g_botSoldiers.find(soldier);
        if (bot != g_botSoldiers.end() && bot->second)
        {
            if (ApplyHit(soldier, info, giverInfo))
            {
                ++g_appliedBotHits;
                OnServerBotHit(soldier, c.giver);
                return;
            }
            ++g_applyFaults;
        }
    }
    g_damage(soldier, info, giverInfo, a3);
}

// Bot bullets this minute, by who they were aimed at.
struct Bullets
{
    int bullets = 0;      // traced
    int noTarget = 0;     // fired with no target in the actor list
    int crossed = 0;      // path within 0.3 m of the target's body
    int zones[3] = {};    // of those: head, torso, legs
    int high = 0, low = 0, side = 0;  // the rest: passed over, under, beside
    std::vector<float> spread;  // degrees between the bot's aim and the bullet's flight
    std::vector<float> miss;    // metres from the body at the closest point
    double pitchBias = 0.0, yawBias = 0.0;  // summed bullet - aim, degrees (+ = up / toward -X from +Z)
    int hits = 0;         // registered hits matched to a crossing
    float damage = 0.0f;
    int crossedSeen = 0, hitsSeen = 0;  // of the crossings / hits: the target within 15 m of you or in your view
    int otherHits = 0;    // registered hits with no matching crossing (explosions, a bystander, ...)
    float otherDamage = 0.0f;
};
Bullets g_toBots, g_toYou;

// A bullet whose path crossed its target's body, waiting for the server's hit (matched within 0.6 s).
struct Crossing
{
    void* shooter = nullptr;
    void* victim = nullptr;
    DWORD time = 0;
    int zone = 1;  // 0 head, 1 torso, 2 legs
    bool seen = false;  // the target was within 15 m of you or in your view (60 degrees, 120 m)
};
std::vector<Crossing> g_crossings;
const char* const kZoneNames[3] = {"head", "torso", "legs"};

// Closest distance between segments p1-q1 and p2-q2 (Ericson, Real-Time Collision Detection 5.1.9); *s, *t = where
// on each (0..1).
float SegmentDistance(const float p1[3], const float q1[3], const float p2[3], const float q2[3], float* s, float* t)
{
    float d1[3], d2[3], r[3];
    for (int i = 0; i < 3; ++i)
    {
        d1[i] = q1[i] - p1[i];
        d2[i] = q2[i] - p2[i];
        r[i] = p1[i] - p2[i];
    }
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float c = dot(d1, r), b = dot(d1, d2), denom = a * e - b * b;
    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; };
    *s = denom > 1e-6f ? clamp01((b * f - c * e) / denom) : 0.0f;
    *t = (b * *s + f) / e;
    if (*t < 0.0f)
    {
        *t = 0.0f;
        *s = clamp01(-c / a);
    }
    else if (*t > 1.0f)
    {
        *t = 1.0f;
        *s = clamp01((b - c) / a);
    }
    float dist2 = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        float diff = (p1[i] + d1[i] * *s) - (p2[i] + d2[i] * *t);
        dist2 += diff * diff;
    }
    return std::sqrt(dist2);
}

bool SeenByYou(const brain::Actor& target, const brain::Actor* human);

// A bot bullet: which actor fired it (the one whose chest is nearest the spawn point), how far it strays from that
// bot's aim, and how close its path comes to the bot's target (the body as a vertical segment for its pose, 0.3 m
// thick).
void TraceBullet(const Event& e, const std::vector<brain::Actor>& actors, const brain::Actor* human, AimFn aimOf,
                 DWORD now)
{
    float speed = std::sqrt(e.vel[0] * e.vel[0] + e.vel[1] * e.vel[1] + e.vel[2] * e.vel[2]);
    if (speed < 100.0f)
        return;  // grenades, rockets
    const brain::Actor* shooter = nullptr;
    float best = 2.0f;
    for (const brain::Actor& a : actors)
    {
        float dx = e.pos[0] - a.pos[0], dy = e.pos[1] - (a.pos[1] + 1.3f), dz = e.pos[2] - a.pos[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < best)
        {
            best = d;
            shooter = &a;
        }
    }
    float yaw = 0.0f, pitch = 0.0f;
    void* target = nullptr;
    if (!shooter || !shooter->isBot || !aimOf(shooter->player, &yaw, &pitch, &target))
        return;
    const brain::Actor* victim = nullptr;
    for (const brain::Actor& a : actors)
        if (a.player == target)
            victim = &a;
    Bullets& b = victim && victim == human ? g_toYou : g_toBots;
    ++b.bullets;
    float dir[3] = {e.vel[0] / speed, e.vel[1] / speed, e.vel[2] / speed};
    float aim[3] = {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    float cosAngle = dir[0] * aim[0] + dir[1] * aim[1] + dir[2] * aim[2];
    cosAngle = cosAngle > 1.0f ? 1.0f : cosAngle < -1.0f ? -1.0f : cosAngle;
    if (b.spread.size() < 20000)
        b.spread.push_back(std::acos(cosAngle) * 57.29578f);
    float bulletPitch = std::asin(dir[1] > 1.0f ? 1.0f : dir[1] < -1.0f ? -1.0f : dir[1]);
    float bulletYaw = std::atan2(-dir[0], dir[2]);
    float yawDiff = std::fmod(bulletYaw - yaw + 3.0f * 3.14159265f, 2.0f * 3.14159265f) - 3.14159265f;
    b.pitchBias += (bulletPitch - pitch) * 57.29578f;
    b.yawBias += yawDiff * 57.29578f;
    if (!victim)
    {
        ++b.noTarget;
        return;
    }
    // The body by pose: standing 0.05-1.8 m, crouched 0.05-1.25 m, prone 0-0.45 m; where the head / legs bands start.
    static const float kBody[3][4] = {{0.05f, 1.8f, 1.45f, 0.85f}, {0.05f, 1.25f, 0.95f, 0.5f}, {0.0f, 0.45f, 9.0f, -1.0f}};
    const float* body = kBody[victim->pose >= 0 && victim->pose <= 2 ? victim->pose : 0];
    float dx = victim->pos[0] - e.pos[0], dz = victim->pos[2] - e.pos[2];
    float reach = std::sqrt(dx * dx + dz * dz) * 1.5f + 2.0f;
    float end[3] = {e.pos[0] + dir[0] * reach, e.pos[1] + dir[1] * reach, e.pos[2] + dir[2] * reach};
    float bottom[3] = {victim->pos[0], victim->pos[1] + body[0], victim->pos[2]};
    float top[3] = {victim->pos[0], victim->pos[1] + body[1], victim->pos[2]};
    float s = 0.0f, t = 0.0f;
    float miss = SegmentDistance(e.pos, end, bottom, top, &s, &t);
    if (miss < 0.3f)
    {
        float height = body[0] + (body[1] - body[0]) * t;
        int zone = height >= body[2] ? 0 : height < body[3] ? 2 : 1;
        bool seen = SeenByYou(*victim, human);
        ++b.crossed;
        ++b.zones[zone];
        b.crossedSeen += seen;
        if (g_crossings.size() < 512)
            g_crossings.push_back(Crossing{shooter->player, victim->player, now, zone, seen});
        return;
    }
    if (b.miss.size() < 20000)
        b.miss.push_back(miss);
    float y = e.pos[1] + dir[1] * reach * s;
    if (y > top[1])
        ++b.high;
    else if (y < bottom[1])
        ++b.low;
    else
        ++b.side;
}

// The crossing that explains a registered hit (same shooter and victim, within 0.6 s), consumed; -1 if none.
int MatchCrossing(void* shooter, void* victim, DWORD now, bool* seen)
{
    for (size_t i = 0; i < g_crossings.size(); ++i)
        if (g_crossings[i].shooter == shooter && g_crossings[i].victim == victim && now - g_crossings[i].time < 600)
        {
            int zone = g_crossings[i].zone;
            *seen = g_crossings[i].seen;
            g_crossings.erase(g_crossings.begin() + i);
            return zone;
        }
    return -1;
}

// Within 15 m of you, or within 120 m and 60 degrees of where you face: your game animates soldiers it can see.
bool SeenByYou(const brain::Actor& target, const brain::Actor* human)
{
    if (!human)
        return false;
    if (&target == human)
        return true;
    float dx = target.pos[0] - human->pos[0], dz = target.pos[2] - human->pos[2];
    float d = std::sqrt(dx * dx + dz * dz);
    if (d < 15.0f)
        return true;
    const float* t = nullptr;
    if (d > 120.0f || !eng::GetSoldierTransform(human->soldier, &t) || !t)
        return false;
    float fl = std::sqrt(t[8] * t[8] + t[10] * t[10]);
    return fl > 0.01f && (dx * t[8] + dz * t[10]) / (d * fl) > 0.5f;
}

float Percentile(std::vector<float>& v, float p)
{
    if (v.empty())
        return 0.0f;
    size_t k = static_cast<size_t>(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

void LogBullets(const char* label, Bullets& b, const char* targetWord)
{
    if (!b.bullets)
        return;
    int aimed = b.bullets - b.noTarget;
    int missed = b.high + b.low + b.side;
    Log("combat (last minute): bots -> %s: %d bullets traced; spread (aim -> bullet) median %.1f deg, 90%% %.1f deg, "
        "average offset %+.2f deg up, %+.2f deg yaw; %d (%.0f%%) crossed %s body (head %d / torso %d / legs %d), "
        "%d registered as hits (%.0f%% of crossings), %.0f damage; misses: over %d / under %d / beside %d, median %.2f m off; %d fired with no target in view; "
        "other hits (no crossing: explosions, bystanders) %d / %.0f damage; targets near you or in your view: %d "
        "crossings, %d hits (%.0f%%), elsewhere: %d crossings, %d hits (%.0f%%)",
        label, b.bullets, Percentile(b.spread, 0.5f), Percentile(b.spread, 0.9f), b.pitchBias / b.bullets,
        b.yawBias / b.bullets, b.crossed,
        aimed ? 100.0f * b.crossed / aimed : 0.0f, targetWord, b.zones[0], b.zones[1], b.zones[2], b.hits,
        b.crossed ? 100.0f * b.hits / b.crossed : 0.0f, b.damage, b.high, b.low, b.side,
        missed ? Percentile(b.miss, 0.5f) : 0.0f, b.noTarget, b.otherHits, b.otherDamage, b.crossedSeen, b.hitsSeen,
        b.crossedSeen ? 100.0f * b.hitsSeen / b.crossedSeen : 0.0f, b.crossed - b.crossedSeen, b.hits - b.hitsSeen,
        b.crossed > b.crossedSeen ? 100.0f * (b.hits - b.hitsSeen) / (b.crossed - b.crossedSeen) : 0.0f);
    b = Bullets{};
}

int ReadInt(const wchar_t* key, int fallback)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    return static_cast<int>(GetPrivateProfileIntW(L"combatlog", key, fallback, path));
}

const char* Describe(void* player, void* human, NameFn nameOf, char* buffer, size_t size)
{
    if (player == human)
        return "you";
    if (const char* name = nameOf(player))
        return name;
    std::snprintf(buffer, size, "%p", player);
    return buffer;
}

void FlushBurst(void* victim, Burst& b, void* human, NameFn nameOf)
{
    if (!b.hits && !b.zeroHits && !b.claims)
        return;
    if (g_hitLines < 400)
    {
        ++g_hitLines;
        char name[24];
        if (b.hits || b.zeroHits)
            Log("combat: you hit %s: %d hit claim%s (%.0f damage claimed), %d applied: health %.0f -> %.0f%s%s",
                Describe(victim, human, nameOf, name, sizeof(name)), b.claims, b.claims == 1 ? "" : "s", b.claimed,
                b.hits + b.zeroHits, b.before, b.after, b.zeroHits ? " (some took no health)" : "",
                b.after <= 0.0f ? " - down" : "");
        else
            Log("combat: you hit %s: %d hit claim%s (%.0f damage claimed), NONE applied by the server",
                Describe(victim, human, nameOf, name, sizeof(name)), b.claims, b.claims == 1 ? "" : "s", b.claimed);
    }
    b = Burst{};
}

}  // namespace

void Init()
{
    InitializeCriticalSection(&g_lock);
    g_enabled = ReadInt(L"Enabled", 1) != 0;
    g_logYourHits = ReadInt(L"LogYourHits", 1) != 0;
    g_windowStart = GetTickCount();
    Log("combat log: %s (per-bot shots, hits, damage, kills each minute%s)", g_enabled ? "on" : "off",
        g_logYourHits ? "; every hit you land on a bot" : "");
    {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
        g_serverBotHits = GetPrivateProfileIntW(L"bots", L"BotHitsOnBotsByServer", 1, path) != 0;
    }
    if (g_enabled || g_serverBotHits)
    {
        if (void* entry = reinterpret_cast<void*>(eng::Address("ServerSoldier_onDamage")))
            g_damage = reinterpret_cast<DamageFn>(InlineHook(entry, 5, reinterpret_cast<const void*>(&DamageDetour)));
        Log("combat log: soldier damage entry %s", g_damage ? "hooked" : "FAILED");
    }
    if (g_serverBotHits && g_damage)
    {
        if (void* claim = reinterpret_cast<void*>(eng::Address("ArbitrationClaim_onMessage")))
            g_claim = reinterpret_cast<ClaimFn>(InlineHook(claim, 6, reinterpret_cast<const void*>(&ClaimDetour)));
        if (!g_claim)
            g_serverBotHits = false;  // without the claim filter bot-vs-bot hits seen by you would count twice
        Log("bot hits on bots: %s", g_serverBotHits ? "applied by the server (players' reports of them ignored)"
                                                    : "left to the game (claim handler hook FAILED)");
    }
}

void OnMessage(const void* message)
{
    if (!g_enabled)
        return;
    const uint32_t* header = static_cast<const uint32_t*>(message);
    const char* m = static_cast<const char*>(message);
    Event e;
    if (header[2] == kShotSpawned && header[1] == kCategoryWeapon)
    {
        e.shot = true;
        e.giver = *reinterpret_cast<void* const*>(m + 0x28);
        if (!e.giver)
            return;
    }
    else if (header[2] == kSoldierDamage && header[1] == kCategoryServerSoldier)
    {
        e.victim = *reinterpret_cast<void* const*>(m + 0x28);
        void* info = *reinterpret_cast<void* const*>(m + 0x2C);
        void* handle = nullptr;
        void* inner = nullptr;
        if (info && eng::ProbePtr(info, &handle) && handle && eng::ProbePtr(handle, &inner) && inner)
            e.giver = static_cast<char*>(inner) - 8;
        e.before = *reinterpret_cast<const float*>(m + 0x34);
        e.after = e.before - *reinterpret_cast<const float*>(m + 0x30);
    }
    else if (header[2] == kProjectileFire && header[1] == kCategoryCollision)
    {
        void* projectile = *reinterpret_cast<void* const*>(m + 0x28);
        const float* p = reinterpret_cast<const float*>(m + 0x30);
        if (!projectile || !eng::ReadFloat(eng::At(projectile, kProjectile_Velocity), &e.vel[0]) ||
            !eng::ReadFloat(eng::At(projectile, kProjectile_Velocity + 4), &e.vel[1]) ||
            !eng::ReadFloat(eng::At(projectile, kProjectile_Velocity + 8), &e.vel[2]))
            return;
        e.bullet = true;
        e.pos[0] = p[0];
        e.pos[1] = p[1];
        e.pos[2] = p[2];
    }
    else if (header[2] == kClientDamageClaim && header[1] == kCategoryArbitration)
    {
        e.claim = true;
        e.victimId = *reinterpret_cast<const int*>(m + 0x28);
        e.giverId = *reinterpret_cast<const int*>(m + 0x30);
        e.claimed = *reinterpret_cast<const float*>(m + 0x50);
    }
    else
        return;
    EnterCriticalSection(&g_lock);
    if (g_events.size() < 8192)
        g_events.push_back(e);
    LeaveCriticalSection(&g_lock);
}

void Tick(const std::vector<brain::Actor>& actors, const std::vector<scoring::Kill>& kills, NameFn nameOf, KitFn kitOf,
          AimFn aimOf)
{
    g_botSoldiers.clear();
    for (const brain::Actor& a : actors)
        g_botSoldiers[a.soldier] = a.isBot;
    if (!g_enabled)
        return;
    DWORD now = GetTickCount();
    void* human = nullptr;
    for (const brain::Actor& a : actors)
    {
        g_ownerOf[a.soldier] = a.player;
        g_ownerSeen[a.soldier] = now;
        if (!a.isBot)
            human = a.player;
    }
    static bool settingLogged = false;
    if (!settingLogged)
    {
        void* synced = nullptr;
        uint8_t allow = 0;
        if (eng::GetSettingsContainer(kTypeInfo_SyncedGameSettings, &synced) && synced &&
            eng::ReadByte(eng::At(synced, 0x1E), &allow))
        {
            settingLogged = true;
            Log("combat log: SyncedGameSettings.AllowClientSideDamageArbitration = %d (your game's hit claims are %s)",
                allow, allow ? "accepted, after the server's checks" : "rejected");
        }
    }
    std::vector<Event> events;
    std::vector<DamageCall> damageCalls;
    EnterCriticalSection(&g_lock);
    events.swap(g_events);
    damageCalls.swap(g_damageCalls);
    LeaveCriticalSection(&g_lock);
    for (const DamageCall& c : damageCalls)
    {
        const brain::Actor* victim = nullptr;
        const brain::Actor* shooter = nullptr;
        for (const brain::Actor& a : actors)
        {
            if (a.soldier == c.soldier)
                victim = &a;
            if (c.giver && a.player == c.giver)
                shooter = &a;
        }
        if (!victim)
            continue;
        int v = victim->isBot ? 0 : 1;
        int g = !c.giver ? 2 : (shooter ? (shooter->isBot ? 0 : 1) : (nameOf(c.giver) ? 0 : 1));
        ++g_damageCounts.calls[v][g];
        g_damageCounts.arbitrable[v][g] += c.arbitrable;
        if (g == 0 && g_damageSamples[v] < 3)
        {
            ++g_damageSamples[v];
            char hex[0x80 * 3 + 1];
            for (int i = 0; i < 0x80; ++i)
                std::snprintf(hex + i * 3, 4, "%02X ", c.raw[i]);
            Log("combat: damage entry sample, bot -> %s%s: %s", v ? "you" : "bot",
                c.arbitrable ? " (client-arbitrable: DROPPED for an AI shooter)" : "", hex);
        }
    }

    int humanId = -1;
    if (human)
        eng::GetPlayerId(human, &humanId);
    const brain::Actor* humanActor = nullptr;
    for (const brain::Actor& a : actors)
        if (a.player == human)
            humanActor = &a;
    for (const Event& e : events)
    {
        if (e.bullet)
        {
            TraceBullet(e, actors, humanActor, aimOf, now);
            continue;
        }
        if (e.shot)
        {
            ++g_stats[e.giver].shots;
            continue;
        }
        if (e.claim)
        {
            if (!g_logYourHits || humanId < 0 || e.giverId != humanId)
                continue;
            for (const brain::Actor& a : actors)
            {
                int id = -1;
                if (a.isBot && eng::GetPlayerId(a.player, &id) && id == e.victimId)
                {
                    Burst& b = g_bursts[a.player];
                    ++b.claims;
                    b.claimed += e.claimed;
                    if (!b.hits && !b.zeroHits)
                        b.before = b.after = 0.0f;
                    b.last = now;
                    break;
                }
            }
            continue;
        }
        auto owner = g_ownerOf.find(e.victim);
        void* victim = owner != g_ownerOf.end() ? owner->second : nullptr;
        float taken = e.before - e.after;
        if (e.giver)
        {
            Stats& s = g_stats[e.giver];
            if (taken > 0.0f)
            {
                ++s.hits;
                s.damage += taken;
            }
            else
                ++s.zeroHits;
            if (victim && victim == human && e.giver != human)
            {
                ++s.hitsOnYou;
                s.damageOnYou += taken;
            }
            // Bot bullets: a registered hit on the bot's target matches a traced crossing (or it was something else).
            if (victim && e.giver != human && nameOf(e.giver))
            {
                Bullets& b = victim == human ? g_toYou : g_toBots;
                bool seen = false;
                int zone = MatchCrossing(e.giver, victim, now, &seen);
                if (zone >= 0)
                {
                    ++b.hits;
                    b.damage += taken;
                    b.hitsSeen += seen;
                }
                else
                {
                    ++b.otherHits;
                    b.otherDamage += taken;
                }
                if (victim == human && g_onYouLines < 300)
                {
                    ++g_onYouLines;
                    float dist = -1.0f;
                    for (const brain::Actor& a : actors)
                        if (a.player == e.giver && humanActor)
                        {
                            float dx = a.pos[0] - humanActor->pos[0], dy = a.pos[1] - humanActor->pos[1],
                                  dz = a.pos[2] - humanActor->pos[2];
                            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        }
                    char name[24];
                    char where[24] = "";
                    if (dist >= 0.0f)
                        std::snprintf(where, sizeof(where), " at %.0f m", dist);
                    Log("combat: %s hit you for %.0f (health %.0f -> %.0f)%s, %s", Describe(e.giver, human, nameOf,
                        name, sizeof(name)), taken, e.before, e.after, where,
                        zone >= 0 ? kZoneNames[zone] : "no traced bullet (explosion, or a bullet at someone else)");
                }
            }
        }
        if (g_logYourHits && human && e.giver == human && victim && victim != human)
        {
            Burst& b = g_bursts[victim];
            if (!b.hits && !b.zeroHits)
                b.before = e.before;
            if (taken > 0.0f)
                ++b.hits;
            else
                ++b.zeroHits;
            b.after = e.after;
            b.last = now;
        }
    }
    for (auto& [victim, b] : g_bursts)
        if ((b.hits || b.zeroHits || b.claims) && now - b.last > 250)
            FlushBurst(victim, b, human, nameOf);

    for (const scoring::Kill& k : kills)
        if (k.killer)
        {
            ++g_stats[k.killer].kills;
            if (k.headshot)
                ++g_stats[k.killer].headshotKills;
        }

    if (now - g_windowStart >= 60000)
    {
        g_windowStart = now;
        Stats bots;
        for (auto& [player, s] : g_stats)
            if (player != human)
            {
                bots.shots += s.shots;
                bots.hits += s.hits;
                bots.damage += s.damage;
                bots.hitsOnYou += s.hitsOnYou;
                bots.damageOnYou += s.damageOnYou;
                bots.kills += s.kills;
                bots.headshotKills += s.headshotKills;
            }
        LogBullets("bots", g_toBots, "the target's");
        LogBullets("you", g_toYou, "your");
        {
            const DamageCounts& d = g_damageCounts;
            Log("combat (last minute): soldier damage entry calls (bullets / explosions that reached a soldier; "
                "client-arbitrable ones are dropped for AI shooters): bot -> bot %d (%d arbitrable), bot -> you %d (%d), "
                "you -> bot %d (%d), no shooter -> bot %d / -> you %d; bot -> bot hits applied by the server %d, "
                "players' reports of bot -> bot hits ignored %d (%d never matched by a server hit), applied as killing "
                "blows %d (your game showed the bot dead)%s",
                d.calls[0][0], d.arbitrable[0][0], d.calls[1][0], d.arbitrable[1][0], d.calls[0][1],
                d.arbitrable[0][1], d.calls[0][2], d.calls[1][2], g_appliedBotHits, g_ignoredBotClaims,
                g_unmatchedClaims, g_lethalBotClaims, g_applyFaults ? " (SOME FAULTED: left to the game)" : "");
            g_damageCounts = DamageCounts{};
            g_appliedBotHits = g_ignoredBotClaims = g_unmatchedClaims = g_lethalBotClaims = g_applyFaults = 0;
        }
        if (bots.shots || bots.hits)
            Log("combat (last minute): ALL BOTS: %d shots, %d hits (%.1f%%), %.0f damage, on you %d hits / %.0f damage, "
                "%d kills (%d headshots)",
                bots.shots, bots.hits, 100.0f * bots.hits / (bots.shots ? bots.shots : 1), bots.damage, bots.hitsOnYou,
                bots.damageOnYou, bots.kills, bots.headshotKills);
        for (auto& [player, s] : g_stats)
        {
            if (!s.shots && !s.hits && !s.kills)
                continue;
            char name[24];
            int kit = player == human ? 0 : kitOf(player);
            int fired = s.shots ? s.shots : 1;
            Log("combat (last minute): %s%s: %d shots, %d hits (%.0f%%), %.0f damage%s, on you %d hits / %.0f damage, "
                "%d kills (%d headshots)",
                Describe(player, human, nameOf, name, sizeof(name)), kit == 4 ? " (sniper)" : kit == 6 ? " (support)" : "",
                s.shots, s.hits, 100.0f * s.hits / fired, s.damage, s.zeroHits ? " (+ hits taking no health)" : "",
                s.hitsOnYou, s.damageOnYou, s.kills, s.headshotKills);
        }
        g_stats.clear();
        for (auto it = g_ownerSeen.begin(); it != g_ownerSeen.end();)  // soldiers gone for over a minute
            if (now - it->second > 60000)
            {
                g_ownerOf.erase(it->first);
                it = g_ownerSeen.erase(it);
            }
            else
                ++it;
        for (auto it = g_bursts.begin(); it != g_bursts.end();)
            it = (it->second.hits || it->second.zeroHits || it->second.claims) ? std::next(it) : g_bursts.erase(it);
    }
    for (size_t i = 0; i < g_crossings.size();)  // crossings the server never turned into a hit
        if (now - g_crossings[i].time > 1000)
            g_crossings.erase(g_crossings.begin() + i);
        else
            ++i;
}

}  // namespace combatlog
