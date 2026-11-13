#include "brain.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine.h"
#include "hardcore.h"
#include "grenades.h"
#include "log.h"

namespace brain {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 2.0f * kPi;

// Heights above the feet by pose - standing (fun-bots Utilities:getTargetHeight), crouched, prone: own eyes, target
// chest / head. Aiming at a standing chest height over a crouched or prone soldier went over its head.
constexpr float kEyeHeight[3] = {1.6f, 1.05f, 0.4f};
constexpr float kChestHeight[3] = {1.1f, 0.75f, 0.25f};
constexpr float kHeadHeight[3] = {1.65f, 1.1f, 0.35f};

int PoseIndex(int pose) { return pose >= eng::kPoseStand && pose <= eng::kPoseProne ? pose : eng::kPoseStand; }

// Line of sight ignores characters (the target's and teammates' bodies), water and ragdolls - like the game's
// spawn visibility test and fun-bots' LOS rays.
constexpr uint32_t kSightFlags = eng::kRay_DontCheckWater | eng::kRay_DontCheckRagdoll | eng::kRay_DontCheckCharacter;

constexpr int kKitSniper = 4;
constexpr int kKitSupport = 6;

struct Settings
{
    bool enabled = true;
    float skill = 0.4f;               // 0..1: aim error, recoil control, reaction time
    float turnRate = 240.0f * kPi / 180.0f;
    float aimErrorMeters = 1.1f;      // miss distance at skill 0.5 (fun-bots BotAimWorsening idea: error / distance)
    float recoilRate = 0.10f;         // simulated climb, radians per second of trigger held
    float recoilMax = 0.05f;          // ~3 degrees (x RecoilPercent)
    float reactionMin = 0.35f;
    float reactionMax = 0.85f;
    float shootDistance = 90.0f;
    float sniperShootDistance = 220.0f;
    bool aimForHead = false;
    bool logEvents = true;
    float halfFov = 60.0f * kPi / 180.0f;  // 120 degrees (fun-bots FovForShooting is 160)
    float grenadeChance = 0.35f;           // per enemy that ducks into cover at 8-25 m
    float spawnGrace = 2.0f;     // a soldier this freshly spawned can't be noticed (then noticing + reaction as usual)
    float sniperSettle = 0.8f;   // a sniper's scope-in / settle time before the first shot at a new target
    float aimSettle = 0.9f;      // seconds after acquiring a target over which the extra aim error fades
    int stuckKillChecks = 10;    // stuck this many checks (1.5 s each) in a row: out by changing class; 0 = never
    float flashMeters = 15.0f;   // a flashbang in sight within this blinds (longest up close, looking at it)
    float flashSeconds = 5.0f;   // blind time at point blank, looking straight at it
    bool smokeBlocksSight = true;
    // Noticing: seconds of unbroken sight it takes to pick out an enemy straight ahead within 10 m, moving, standing
    // (see NoticeTime for what makes it longer or shorter).
    float noticeTime = 0.45f;
    // Hearing (outside the field of view): gunfire, and footsteps by how fast the enemy moves. A heard enemy is
    // turned toward, not engaged - it still has to be seen and noticed.
    float hearGunfire = 30.0f;
    float hearSprint = 10.0f;
    float hearRun = 5.0f;
    float hearWalk = 2.0f;
    float botAimError = 1.0f;  // aim error against other bots, x
};
Settings g_settings;

int ReadInt(const wchar_t* key, int fallback)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    return static_cast<int>(GetPrivateProfileIntW(L"brain", key, fallback, path));
}

// --- small math ------------------------------------------------------------------------------------------------
float Rand(State& s)  // [0, 1)
{
    uint32_t x = s.rng ? s.rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s.rng = x;
    return static_cast<float>(x & 0xFFFFFF) / 16777216.0f;
}
float Rand(State& s, float lo, float hi) { return lo + (hi - lo) * Rand(s); }

float WrapTwoPi(float a)
{
    a = std::fmod(a, kTwoPi);
    return a < 0.0f ? a + kTwoPi : a;
}

float AngleDiff(float from, float to)  // shortest signed to - from
{
    float d = std::fmod(to - from + kPi, kTwoPi);
    if (d < 0.0f)
        d += kTwoPi;
    return d - kPi;
}

// fun-bots BotAiming: yaw = atan2(dz, dx) - pi/2 in [0, 2pi); forward(yaw) = (-sin yaw, 0, cos yaw).
float YawTo(float dx, float dz) { return WrapTwoPi(std::atan2(dz, dx) - kPi / 2.0f); }

float Distance(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Distance2D(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

// fun-bots BotAiming _GetGrenadePitch (measured for 1-25 m), linearly interpolated: radians up for a lob landing at
// `distance` metres.
float GrenadePitch(float distance)
{
    static const float kTable[][2] = {{1.0f, 1.5604f},  {5.0f, 1.4643f},  {10.0f, 1.3526f}, {15.0f, 1.2305f},
                                      {20.0f, 1.0751f}, {22.0f, 0.9913f}, {24.0f, 0.8570f}, {25.0f, 0.7505f}};
    if (distance <= kTable[0][0])
        return kTable[0][1];
    for (size_t i = 1; i < sizeof(kTable) / sizeof(kTable[0]); ++i)
        if (distance <= kTable[i][0])
        {
            float t = (distance - kTable[i - 1][0]) / (kTable[i][0] - kTable[i - 1][0]);
            return kTable[i - 1][1] + (kTable[i][1] - kTable[i - 1][1]) * t;
        }
    return kTable[7][1];
}

// Line of sight from the eyes to the target's chest (lower when it crouches or lies prone) - not through smoke.
bool Visible(const float eye[3], const Actor& target)
{
    float to[3] = {target.pos[0], target.pos[1] + kChestHeight[PoseIndex(target.pose)], target.pos[2]};
    bool blocked = true;
    return eng::RayBlocked(eye, to, kSightFlags, &blocked) && !blocked &&
           !(g_settings.smokeBlocksSight && grenades::SmokeBlocks(eye, to));
}

// How long a flashbang at `pos` blinds a bot with its eyes at `eye` looking along `yaw`: none behind a wall or beyond
// FlashMeters; up to FlashSeconds at point blank looking straight at it, falling off with distance, and to 30% with
// its back turned (a flashbang is too bright to escape by looking away up close).
float FlashSeconds(const float eye[3], float yaw, const float pos[3], float* outDistance, float* outOff)
{
    float d = Distance(eye, pos);
    float off = std::fabs(AngleDiff(yaw, YawTo(pos[0] - eye[0], pos[2] - eye[2])));
    *outDistance = d;
    *outOff = off;
    if (d > g_settings.flashMeters)
        return 0.0f;
    float at[3] = {pos[0], pos[1] + 0.3f, pos[2]};
    bool blocked = true;
    if (!eng::RayBlocked(eye, at, kSightFlags, &blocked) || blocked)
        return 0.0f;
    float closeness = 1.0f - (d / g_settings.flashMeters) * (d / g_settings.flashMeters);
    float facing = off <= kPi / 4.0f ? 1.0f : 1.0f - 0.7f * (off - kPi / 4.0f) / (kPi * 0.75f);
    return g_settings.flashSeconds * closeness * facing;
}

// Is the way ahead (yaw) clear for `length` metres at `height` above the feet?
bool Clear(const float feet[3], float yaw, float height, float length)
{
    float from[3] = {feet[0], feet[1] + height, feet[2]};
    float to[3] = {from[0] - std::sin(yaw) * length, from[1], from[2] + std::cos(yaw) * length};
    bool blocked = true;
    return eng::RayBlocked(from, to, kSightFlags, &blocked) && !blocked;
}

// Water `length` metres ahead (probed from the bot's own height down 4 m): the sea at the map edge reads as wide open
// space to the obstacle rays, and a stuck bot's detour walked straight into it.
bool WetAhead(const float feet[3], float yaw, float length)
{
    float p[3] = {feet[0] - std::sin(yaw) * length, feet[1], feet[2] + std::cos(yaw) * length};
    return eng::WaterDepth(p) > 0.4f;
}

const Actor* FindActor(const std::vector<Actor>& actors, void* player)
{
    for (const Actor& a : actors)
        if (a.player == player)
            return &a;
    return nullptr;
}

// --- input ---------------------------------------------------------------------------------------------------
struct Output
{
    float throttle = 0.0f;  // +forward
    float strafe = 0.0f;    // +right
    bool fire = false;
    bool sprint = false;
    bool jump = false;
    bool reload = false;
    bool throwGrenade = false;
    bool zoom = false;  // aim down sights
};

void SetDigital(char* input, int action, bool on)
{
    auto* bits = reinterpret_cast<uint64_t*>(input + eng::kInput_DigitalBits);
    uint64_t bit = 1ull << (action - 12);
    *bits = on ? (*bits | bit) : (*bits & ~bit);
}

void Write(void* inputPtr, const State& s, const Output& o)
{
    auto* input = static_cast<char*>(inputPtr);
    auto* analog = reinterpret_cast<float*>(input + eng::kInput_Analog);
    analog[eng::EIAThrottle] = o.throttle;
    analog[eng::EIAStrafe] = o.strafe;
    analog[eng::EIAFire] = o.fire ? 1.0f : 0.0f;
    SetDigital(input, eng::EIASprint, o.sprint);
    SetDigital(input, eng::EIAJump, o.jump);
    SetDigital(input, eng::EIAReload, o.reload);
    SetDigital(input, eng::EIAThrowGrenade, o.throwGrenade);
    SetDigital(input, eng::EIAZoom, o.zoom);
    *reinterpret_cast<float*>(input + eng::kInput_Yaw) = s.yaw;
    *reinterpret_cast<float*>(input + eng::kInput_Pitch) = s.pitch;
    *reinterpret_cast<uint32_t*>(input + eng::kInput_Flags) |= 1u;  // AuthoritativeAiming
}

// World direction -> throttle/strafe relative to the current aim (forward = (-sin, cos), right = (cos, sin)).
void MoveToward(const State& s, float moveYaw, float speed, Output& o)
{
    float dx = -std::sin(moveYaw), dz = std::cos(moveYaw);
    float fx = -std::sin(s.yaw), fz = std::cos(s.yaw);
    float rx = std::cos(s.yaw), rz = std::sin(s.yaw);
    o.throttle = (dx * fx + dz * fz) * speed;
    o.strafe = (dx * rx + dz * rz) * speed;
}

void TurnToward(State& s, float yaw, float pitch, float dt)
{
    float step = g_settings.turnRate * dt;
    float d = AngleDiff(s.yaw, yaw);
    s.yaw = WrapTwoPi(std::fabs(d) <= step ? yaw : s.yaw + (d > 0.0f ? step : -step));
    float pd = pitch - s.pitch;
    s.pitch += std::fabs(pd) <= step ? pd : (pd > 0.0f ? step : -step);
    if (s.pitch > 1.4f)
        s.pitch = 1.4f;
    if (s.pitch < -1.4f)
        s.pitch = -1.4f;
}

// --- perception ------------------------------------------------------------------------------------------------
// Seconds of unbroken sight it takes to pick `a` out (NoticeMs straight ahead within 10 m, scaled by skill): up to 3x
// at the edge of the field of view, +1x per 25 m beyond 10 m, 1.5x crouched / 2.5x prone, 1.3x standing still, 0.8x
// sprinting - and a third of that while it is firing (muzzle flash, tracers).
float NoticeTime(const Actor& a, float distance, float offCenter)
{
    float t = g_settings.noticeTime * (1.5f - g_settings.skill);
    float edge = offCenter / g_settings.halfFov;
    t *= 1.0f + 2.0f * edge * edge;
    t *= 1.0f + std::fmax(0.0f, distance - 10.0f) / 25.0f;
    if (a.pose == eng::kPoseCrouch)
        t *= 1.5f;
    else if (a.pose == eng::kPoseProne)
        t *= 2.5f;
    if (a.speed < 0.5f)
        t *= 1.3f;
    else if (a.speed > 5.0f)
        t *= 0.8f;
    if (a.sinceFired < 1.0f)
        t *= 0.35f;
    return t;
}

// How far away `a` can be heard: its gunfire, else its footsteps by speed (crouched or prone and slow: silent).
float HearingRange(const Actor& a)
{
    if (a.sinceFired < 0.5f)
        return g_settings.hearGunfire;
    if (a.state != eng::kStateOnGround)
        return 0.0f;
    if (a.speed > 5.0f)
        return g_settings.hearSprint;
    if (a.speed > 2.0f)
        return a.pose == eng::kPoseStand ? g_settings.hearRun : g_settings.hearWalk;
    if (a.speed > 0.5f)
        return a.pose == eng::kPoseStand ? g_settings.hearWalk : 0.0f;
    return 0.0f;
}

Awareness& AwarenessOf(State& s, void* player)
{
    for (Awareness& a : s.awareness)
        if (a.player == player)
            return a;
    s.awareness.push_back(Awareness{player, 0.0f});
    return s.awareness.back();
}

void Perceive(State& s, const char* name, int team, int kitType, const float feet[3], const float eye[3],
              const std::vector<Actor>& actors, float dt)
{
    // Follow the current target while it lives.
    if (s.target)
    {
        const Actor* t = FindActor(actors, s.target);
        if (!t || t->team == team)
        {
            if (g_settings.logEvents)
                Log("brain %s: target gone (dead or left)", name);
            s.target = nullptr;
        }
        else if (s.sinceSeen < 0.5f)
            std::memcpy(s.targetPos, t->pos, sizeof(s.targetPos));  // in sight: track it every tick, not per ray
    }
    s.sinceSeen += dt;
    s.hearCooldown -= dt;

    s.visTimer -= dt;
    if (s.visTimer > 0.0f)
        return;
    float elapsed = s.checkInterval;
    s.checkInterval = s.visTimer = Rand(s, 0.15f, 0.25f);  // staggers the rays across bots and ticks

    float range = kitType == kKitSniper ? g_settings.sniperShootDistance : g_settings.shootDistance;

    // Current target first, so a bot doesn't flick between enemies.
    if (s.target)
    {
        const Actor* t = FindActor(actors, s.target);
        if (t && Distance(feet, t->pos) <= range * 1.2f && Visible(eye, *t))
        {
            std::memcpy(s.targetPos, t->pos, sizeof(s.targetPos));
            s.sinceSeen = 0.0f;
            AwarenessOf(s, s.target).level = 1.0f;
            return;
        }
    }

    // Otherwise the nearest enemies in the field of view (up to 3 rays per check). An enemy outside it that can be
    // heard is turned toward instead (at most every 3 s, and not mid-fight): it still has to be seen.
    const Actor* best[3] = {};
    float bestDist[3] = {1e9f, 1e9f, 1e9f};
    float bestOff[3] = {};
    const Actor* heard = nullptr;
    float heardDist = 1e9f;
    for (const Actor& a : actors)
    {
        if (a.team == team || a.team == 0)
            continue;
        float d = Distance(feet, a.pos);
        if (d > range)
            continue;
        float bearing = YawTo(a.pos[0] - feet[0], a.pos[2] - feet[2]);
        float off = std::fabs(AngleDiff(s.yaw, bearing));
        if (off > g_settings.halfFov)
        {
            if (d <= HearingRange(a) && d < heardDist)
            {
                heard = &a;
                heardDist = d;
            }
            continue;
        }
        for (int i = 0; i < 3; ++i)
            if (d < bestDist[i])
            {
                for (int k = 2; k > i; --k)
                {
                    best[k] = best[k - 1];
                    bestDist[k] = bestDist[k - 1];
                    bestOff[k] = bestOff[k - 1];
                }
                best[i] = &a;
                bestDist[i] = d;
                bestOff[i] = off;
                break;
            }
    }
    if (heard && s.hearCooldown <= 0.0f && !(s.target && s.sinceSeen < 2.5f))
    {
        s.hearCooldown = 3.0f;
        s.lookYaw = YawTo(heard->pos[0] - feet[0], heard->pos[2] - feet[2]);
        s.lookTimer = 1.5f;
        if (g_settings.logEvents)
            Log("brain %s: heard %s %p (%s) %.0f m away; turning to look", name, heard->isBot ? "bot" : "player",
                heard->player, heard->sinceFired < 0.5f ? "gunfire" : "footsteps", heardDist);
    }

    // Sight builds awareness over the time it takes to pick each one out; out of sight it fades over 3 s.
    const Actor* noticed = nullptr;
    float noticedDist = 0.0f;
    void* seen[3] = {};
    for (int i = 0; i < 3 && best[i]; ++i)
    {
        if (!Visible(eye, *best[i]))
            continue;
        seen[i] = best[i]->player;
        // A soldier that has just spawned can't be picked out yet (SpawnGraceMs): no spawn kills, even when the
        // game's fall-back spawn puts you in an enemy's view.
        if (best[i]->spawnAge < g_settings.spawnGrace)
            continue;
        Awareness& aw = AwarenessOf(s, best[i]->player);
        aw.level = std::fmin(1.0f, aw.level + elapsed / NoticeTime(*best[i], bestDist[i], bestOff[i]));
        if (aw.level >= 1.0f && !noticed)
        {
            noticed = best[i];
            noticedDist = bestDist[i];
        }
    }
    for (size_t i = 0; i < s.awareness.size();)
    {
        Awareness& aw = s.awareness[i];
        bool inSight = aw.player == seen[0] || aw.player == seen[1] || aw.player == seen[2];
        if (!inSight)
            aw.level -= elapsed / 3.0f;
        if (aw.level <= 0.0f || !FindActor(actors, aw.player))
            s.awareness.erase(s.awareness.begin() + i);
        else
            ++i;
    }
    if (!noticed)
        return;

    if (noticed->player != s.target)
    {
        if (!s.target || s.sinceSeen > 1.0f)
        {
            // Noticed: now aim and decide to shoot.
            s.reaction = Rand(s, g_settings.reactionMin, g_settings.reactionMax) * (1.5f - g_settings.skill);
            // A sniper scopes in and settles before the first shot, longer the farther the target.
            if (kitType == kKitSniper)
                s.reaction += g_settings.sniperSettle * Rand(s, 0.75f, 1.25f) + noticedDist / 250.0f;
        }
        // Snipers go for the head about as often as their skill (always, with AimForHead); others aim center mass.
        s.headAim = g_settings.aimForHead || (kitType == kKitSniper && Rand(s) < g_settings.skill);
        s.settle = 1.0f;
        if (g_settings.logEvents)
            Log("brain %s: engaging %s %p at %.0f m (%s%s)", name, noticed->isBot ? "bot" : "player", noticed->player,
                noticedDist, eng::PoseName(noticed->pose), noticed->sinceFired < 1.0f ? ", firing" : "");
        s.target = noticed->player;
        s.fireOn = false;
        s.burstTimer = 0.0f;
    }
    std::memcpy(s.targetPos, noticed->pos, sizeof(s.targetPos));
    s.sinceSeen = 0.0f;
}

// --- firing ----------------------------------------------------------------------------------------------------
// Friendly fire is on (Hardcore, or ProCon): a teammate between the bot's eye and what it aims at, within what its spread
// could hit.
bool FriendInLine(void* self, int team, const float eye[3], const float aim[3], const std::vector<Actor>& actors)
{
    const float d[3] = {aim[0] - eye[0], aim[1] - eye[1], aim[2] - eye[2]};
    const float lengthSq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (lengthSq < 0.01f)
        return false;
    for (const Actor& a : actors)
    {
        if (a.player == self || a.team != team)
            continue;
        const float v[3] = {a.pos[0] - eye[0], a.pos[1] + 0.9f - eye[1], a.pos[2] - eye[2]};
        const float t = (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / lengthSq;
        if (t <= 0.0f || t >= 1.0f)
            continue;
        const float off[3] = {v[0] - d[0] * t, (v[1] - d[1] * t) * 0.6f, v[2] - d[2] * t};  // a soldier is taller than wide
        const float reach = 0.6f + 0.02f * t * std::sqrt(lengthSq);
        if (off[0] * off[0] + off[1] * off[1] + off[2] * off[2] < reach * reach)
            return true;
    }
    return false;
}

// And a teammate where a grenade would land.
bool FriendNear(void* self, int team, const float at[3], float radius, const std::vector<Actor>& actors)
{
    for (const Actor& a : actors)
    {
        const float dx = a.pos[0] - at[0], dy = a.pos[1] - at[1], dz = a.pos[2] - at[2];
        if (a.player != self && a.team == team && dx * dx + dy * dy + dz * dz < radius * radius)
            return true;
    }
    return false;
}

void Fire(State& s, int kitType, float distance, float aimDelta, float dt, Output& o)
{
    s.reaction -= dt;
    bool canShoot = s.target && s.sinceSeen < 0.35f && s.reaction <= 0.0f && std::fabs(aimDelta) < 0.12f;
    if (!canShoot)
    {
        s.fireOn = false;
        s.burstTimer = 0.0f;
        return;
    }
    // Burst rhythm by kit (fun-bots fireCycle / pauseCycle): sniper single taps, support longer bursts. Kept short:
    // the weapon's spread blooms during a long burst (bots sprayed 400-600 rounds a minute each and hit with 1-2%).
    s.burstTimer -= dt;
    if (s.burstTimer <= 0.0f)
    {
        s.fireOn = !s.fireOn;
        if (kitType == kKitSniper)
            s.burstTimer = s.fireOn ? 0.1f : Rand(s, 1.2f, 1.8f);
        else if (kitType == kKitSupport)
            s.burstTimer = s.fireOn ? Rand(s, 0.4f, 0.8f) : Rand(s, 0.3f, 0.5f);
        else if (distance > 40.0f)
            s.burstTimer = s.fireOn ? Rand(s, 0.1f, 0.2f) : Rand(s, 0.35f, 0.7f);  // taps at range
        else if (distance > 15.0f)
            s.burstTimer = s.fireOn ? Rand(s, 0.2f, 0.35f) : Rand(s, 0.25f, 0.5f);
        else
            s.burstTimer = s.fireOn ? Rand(s, 0.3f, 0.5f) : Rand(s, 0.15f, 0.3f);
    }
    o.fire = s.fireOn;
    if (o.fire)
    {
        s.triggerTime += dt;
        s.sinceFired = 0.0f;
    }
}

// --- movement --------------------------------------------------------------------------------------------------
int g_goalCounts[kGoalKinds] = {};

// An enemy to go after: one of the nearest three, the nearest most often. Always the nearest sent a whole team down
// the same route as one pack (bots know where everyone is, like fun-bots' TDM behaviour without paths).
const Actor* PickEnemy(State& s, int team, const float feet[3], const std::vector<Actor>& actors, float* outDist)
{
    const Actor* best[3] = {};
    float dist[3] = {1e9f, 1e9f, 1e9f};
    for (const Actor& a : actors)
    {
        if (a.team == team || a.team == 0)
            continue;
        float d = Distance(feet, a.pos);
        for (int k = 0; k < 3; ++k)
            if (d < dist[k])
            {
                for (int m = 2; m > k; --m)
                {
                    dist[m] = dist[m - 1];
                    best[m] = best[m - 1];
                }
                dist[k] = d;
                best[k] = &a;
                break;
            }
    }
    int count = best[2] ? 3 : best[1] ? 2 : best[0] ? 1 : 0;
    if (!count)
        return nullptr;
    float r = Rand(s);
    int k = r < 0.55f || count == 1 ? 0 : r < 0.85f || count == 2 ? 1 : 2;
    *outDist = dist[k];
    return best[k];
}

// Somewhere 12-28 m to one side of the line to `enemy`, part of the way there, on a learned nav node: coming at it
// from the flank. False when there's no node near either side.
bool FlankPoint(State& s, const float feet[3], const Actor& enemy, float out[3])
{
    float dx = enemy.pos[0] - feet[0], dz = enemy.pos[2] - feet[2];
    float flat = std::sqrt(dx * dx + dz * dz);
    if (flat < 15.0f)
        return false;  // too close to swing wide
    dx /= flat;
    dz /= flat;
    float along = flat * Rand(s, 0.5f, 0.8f);
    float offset = std::fmin(Rand(s, 12.0f, 28.0f), flat * 0.7f);
    float side = Rand(s) < 0.5f ? -1.0f : 1.0f;
    for (int attempt = 0; attempt < 2; ++attempt, side = -side)
    {
        float p[3] = {feet[0] + dx * along - dz * side * offset, enemy.pos[1], feet[2] + dz * along + dx * side * offset};
        if (nav::NearestNode(p, 12.0f, out))
            return true;
    }
    return false;
}

// A random spot on the map 25-140 m away, the one of four farthest from its teammates.
bool RoamPoint(State& s, void* self, int team, const float feet[3], const std::vector<Actor>& actors, float out[3])
{
    bool found = false;
    float bestSpace = -1.0f;
    for (int i = 0; i < 4; ++i)
    {
        float p[3];
        Rand(s);
        if (!nav::RandomNode(s.rng, feet, 25.0f, 140.0f, p))
            continue;
        float space = 1e9f;
        for (const Actor& a : actors)
            if (a.team == team && a.player != self)
                space = std::fmin(space, Distance2D(p, a.pos));
        if (space > bestSpace)
        {
            bestSpace = space;
            std::memcpy(out, p, sizeof(p));
            found = true;
        }
    }
    return found;
}

// --- game-mode objectives (Sector Control flags) ----------------------------------------------------------------
std::vector<Objective> g_objectives;

bool EnemyNear(int team, const float p[3], float radius, const std::vector<Actor>& actors)
{
    for (const Actor& a : actors)
        if (a.team != team && a.team != 0 && Distance2D(p, a.pos) < radius && std::fabs(a.pos[1] - p[1]) < 6.0f)
            return true;
    return false;
}

bool ObjectiveWanted(int index, int team, const std::vector<Actor>& actors)
{
    if (index < 0 || index >= static_cast<int>(g_objectives.size()))
        return false;
    const Objective& o = g_objectives[index];
    return o.owner != team || EnemyNear(team, o.pos, 15.0f, actors);
}

// A spot inside the objective's area to stand on.
void ObjectiveSpot(State& s, const Objective& o, float out[3])
{
    float yaw = Rand(s, 0.0f, kTwoPi);
    float dist = Rand(s, 0.0f, o.radius * 0.6f);
    out[0] = o.pos[0] - std::sin(yaw) * dist;
    out[1] = o.pos[1];
    out[2] = o.pos[2] + std::cos(yaw) * dist;
    s.atSpot = false;
}

// Where a bot standing on a flag looks: down one of the ways in - a learned node 10-35 m out (people walked there), one it
// can see likelier, and likelier toward the flags its team doesn't hold (where enemies come from); not the way it just
// watched. With no routes learned yet: a quarter turn either way.
float WatchYaw(State& s, int objective, int team, const float eye[3])
{
    const Objective& ob = g_objectives[objective];
    float best = WrapTwoPi(s.watchYaw + (Rand(s) < 0.5f ? -1.0f : 1.0f) * Rand(s, 1.0f, 2.0f));
    float bestScore = -1e9f;
    for (int i = 0; i < 6; ++i)
    {
        float p[3];
        Rand(s);
        if (!nav::RandomNode(s.rng, ob.pos, 10.0f, 35.0f, p))
            continue;
        const float yaw = YawTo(p[0] - eye[0], p[2] - eye[2]);
        float score = Rand(s, 0.0f, 0.8f);
        float toward = -1.0f;
        for (size_t k = 0; k < g_objectives.size(); ++k)
            if (static_cast<int>(k) != objective && g_objectives[k].owner != team)
                toward = std::fmax(toward, std::cos(AngleDiff(yaw, YawTo(g_objectives[k].pos[0] - ob.pos[0],
                                                                         g_objectives[k].pos[2] - ob.pos[2]))));
        score += 0.6f * toward;
        const float to[3] = {p[0], p[1] + 1.2f, p[2]};
        bool blocked = true;
        if (eng::RayBlocked(eye, to, kSightFlags, &blocked) && !blocked)
            score += 1.0f;
        if (std::fabs(AngleDiff(yaw, s.watchYaw)) < 0.6f)
            score -= 0.8f;
        if (score > bestScore)
        {
            bestScore = score;
            best = yaw;
        }
    }
    return best;
}

// Which objective to head for this time, or -1: a share of each team plays the objectives (roamers most, hunters least),
// the rest hunt as before. Ones the team doesn't hold weigh most, then its own with an enemy near; nearer ones are
// likelier, ones with teammates already at them less likely. A bot that was on its way to one keeps at it while it's
// still wanted - less often once it has stood on it a minute and it still isn't theirs.
int PickObjective(State& s, void* self, int team, const float feet[3], const std::vector<Actor>& actors)
{
    if (g_objectives.empty())
        return -1;
    const bool stale = s.heldObjective == s.objective && s.heldFor >= 60.0f;
    if (s.goalKind == kGoalObjective && ObjectiveWanted(s.objective, team, actors) && Rand(s) < (stale ? 0.3f : 0.85f))
        return s.objective;
    static const float kOdds[3] = {0.45f, 0.6f, 0.75f};
    if (Rand(s) >= kOdds[s.style])
        return -1;
    const int count = static_cast<int>(std::min<size_t>(g_objectives.size(), 16));
    float weights[16] = {};
    float total = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        const Objective& o = g_objectives[i];
        const bool ours = o.owner == team;
        float w = !ours ? 1.0f : EnemyNear(team, o.pos, 15.0f, actors) ? 1.5f : 0.15f;  // a few stay to guard their own
        w /= 1.0f + Distance2D(feet, o.pos) / 60.0f;
        int mates = 0;
        for (const Actor& a : actors)
            if (a.team == team && a.player != self && Distance2D(a.pos, o.pos) < 12.0f)
                ++mates;
        w /= 1.0f + 0.3f * static_cast<float>(mates);
        if (stale && i == s.heldObjective)
            w *= 0.2f;
        weights[i] = w;
        total += w;
    }
    float r = Rand(s) * total;
    for (int i = 0; i < count; ++i)
    {
        r -= weights[i];
        if (r <= 0.0f)
            return i;
    }
    return count - 1;
}

void PickGoal(State& s, void* self, int team, const float feet[3], const std::vector<Actor>& actors)
{
    if (s.style < 0)
        s.style = static_cast<int>(Rand(s) * 3.0f) % 3;  // 0 hunter, 1 flanker, 2 roamer
    const int objective = PickObjective(s, self, team, feet, actors);
    if (objective >= 0)
    {
        ObjectiveSpot(s, g_objectives[objective], s.goal);
        if (s.goalKind != kGoalObjective || s.objective != objective)
            ++g_goalCounts[kGoalObjective];
        s.goalKind = kGoalObjective;
        s.objective = objective;
        s.hasGoal = true;
        s.goalTimer = Rand(s, 45.0f, 75.0f);
        return;
    }
    s.objective = -1;
    // hunt / flank / roam odds by leaning; with two or more teammates within 8 m it rather goes its own way.
    static const float kOdds[3][kGoalKinds] = {{0.70f, 0.20f, 0.10f}, {0.30f, 0.55f, 0.15f}, {0.25f, 0.20f, 0.55f}};
    int crowd = 0;
    for (const Actor& a : actors)
        if (a.team == team && a.player != self && Distance(feet, a.pos) < 8.0f)
            ++crowd;
    float odds[kGoalKinds] = {kOdds[s.style][0], kOdds[s.style][1], kOdds[s.style][2]};
    if (crowd >= 2)
    {
        odds[kGoalHunt] *= 0.4f;
        odds[kGoalRoam] *= 1.5f;
    }
    float r = Rand(s) * (odds[0] + odds[1] + odds[2]);
    int kind = r < odds[kGoalHunt] ? kGoalHunt : r < odds[kGoalHunt] + odds[kGoalFlank] ? kGoalFlank : kGoalRoam;

    float enemyDist = 0.0f;
    const Actor* enemy = PickEnemy(s, team, feet, actors, &enemyDist);
    bool placed = false;
    if (kind == kGoalFlank)
        placed = enemy && FlankPoint(s, feet, *enemy, s.goal);
    else if (kind == kGoalRoam)
        placed = RoamPoint(s, self, team, feet, actors, s.goal);
    if (!placed && enemy)
    {
        kind = kGoalHunt;
        float jitter = enemyDist > 20.0f ? 6.0f : 2.0f;
        s.goal[0] = enemy->pos[0] + Rand(s, -jitter, jitter);
        s.goal[1] = enemy->pos[1];
        s.goal[2] = enemy->pos[2] + Rand(s, -jitter, jitter);
    }
    else if (!placed)
    {
        kind = kGoalRoam;
        float yaw = Rand(s, 0.0f, kTwoPi);
        float dist = Rand(s, 15.0f, 35.0f);
        s.goal[0] = feet[0] - std::sin(yaw) * dist;
        s.goal[1] = feet[1];
        s.goal[2] = feet[2] + std::cos(yaw) * dist;
    }
    s.goalKind = kind;
    ++g_goalCounts[kind];
    s.hasGoal = true;
    s.goalTimer = kind == kGoalHunt ? Rand(s, 6.0f, 10.0f) : Rand(s, 12.0f, 18.0f);
}

// Probes ahead along moveYaw: jump low obstacles, steer around tall ones. Returns the yaw to walk. On a learned route
// (onPath) it only jumps: a human walked that link, so a blocked chest ray is usually a door frame or a corner, and
// detouring would pull the bot off the route (getting stuck penalises the link instead).
float AvoidObstacles(State& s, const float feet[3], float moveYaw, bool onPath, float dt)
{
    if (s.detourTimer > 0.0f)
    {
        s.detourTimer -= dt;
        moveYaw = s.detourYaw;
    }
    s.probeTimer -= dt;
    if (s.probeTimer > 0.0f)
        return moveYaw;
    s.probeTimer = 0.15f;

    bool kneeClear = Clear(feet, moveYaw, 0.45f, 1.4f);
    bool chestClear = Clear(feet, moveYaw, 1.2f, 1.4f);
    bool wet = !onPath && WetAhead(feet, moveYaw, 2.0f);  // routes only use dry nodes
    if (!kneeClear && chestClear && !wet)
    {
        s.jumpHold = 0.2f;  // a low wall or step: jump it
        return moveYaw;
    }
    if ((chestClear && !wet) || onPath)
        return moveYaw;
    // Blocked: first clear direction fanning out from the wanted one.
    static const float kOffsets[] = {0.8f, -0.8f, 1.6f, -1.6f, 2.4f, -2.4f};
    float flip = Rand(s) < 0.5f ? 1.0f : -1.0f;
    for (float offset : kOffsets)
    {
        float yaw = WrapTwoPi(moveYaw + offset * flip);
        if (Clear(feet, yaw, 1.0f, 2.5f) && !WetAhead(feet, yaw, 2.5f))
        {
            s.detourYaw = yaw;
            s.detourTimer = Rand(s, 0.8f, 1.4f);
            return yaw;
        }
    }
    s.detourYaw = WrapTwoPi(moveYaw + kPi);  // boxed in: turn around
    s.detourTimer = 1.0f;
    return s.detourYaw;
}

// Water within `length` of the bot in any of 8 directions.
bool WaterAround(const float feet[3], float length)
{
    for (int i = 0; i < 8; ++i)
        if (WetAhead(feet, i * (kTwoPi / 8.0f), length))
            return true;
    return false;
}

// The most open direction around the bot, leaning toward wantYaw: 12 directions at chest height, each tested at 3,
// 7 and 12 m (stopping at the first block), scored by free length + 2.5 x cos(angle to wantYaw). A doorway reads as
// a long free ray, so a bot boxed in a room picks the way out instead of circling. `avoidYaw` (when not null): the way
// it already failed to get out - the directions within 45 degrees of it lose 8.
float OpenestDirection(State& s, const float feet[3], float wantYaw, const float* avoidYaw = nullptr)
{
    static const float kLengths[] = {3.0f, 7.0f, 12.0f};
    float bestYaw = wantYaw, bestScore = -1e9f;
    for (int i = 0; i < 12; ++i)
    {
        float yaw = WrapTwoPi(wantYaw + i * (kTwoPi / 12.0f));
        float free = 0.0f;
        for (float length : kLengths)
        {
            if (!Clear(feet, yaw, 1.2f, length))
                break;
            free = length;
        }
        float score = free + 2.5f * std::cos(AngleDiff(yaw, wantYaw)) + Rand(s, 0.0f, 0.5f);
        // Open water anywhere along the way (not just at its far end, which can be land across a bay): last resort.
        bool wet = WetAhead(feet, yaw, 2.5f);
        for (float length : kLengths)
            wet = wet || (length <= free && WetAhead(feet, yaw, length));
        if (wet)
            score -= 100.0f;
        if (avoidYaw && std::fabs(AngleDiff(yaw, *avoidYaw)) < kPi / 4.0f)
            score -= 8.0f;
        if (score > bestScore)
        {
            bestScore = score;
            bestYaw = yaw;
        }
    }
    return bestYaw;
}

// Returns true when the bot hasn't moved 0.6 m in the last 1.5 s while trying to; it then heads another way for 2-3 s,
// escalating while it stays stuck: first the most open direction, then back where it came from (it walked in from
// there, so it can usually walk out), then open directions other than the one that just failed. It jumps on every
// other try (pressing jump at a wall over and over was all Doug did for 5 minutes on Novi Grad), never with water
// within 2 m. A spell lasts until the bot is 5 m from where it began; found stuck 6 times in it (9 s), or 12 s into it
// with 3 stuck checks, the route nodes at the spot are marked as a trap (nav::MarkTrap): routes stop leading there.
// Found stuck StuckChangeClassSeconds' worth of checks, or LongSpellChecks into a spell with 3 of them, it sets
// s.changeClass. The second half of each rule is for a bot that shuffles about without ever getting 5 m away: on
// Somalia Stronghold four bots spent 15-24 s at one spot with only 4 checks that found them still.
// A check with another soldier within 1.5 m (`body`, its feet) counts for none of it: two bots pushing at each other
// in a doorway aren't stuck on the map (the probes see through bodies, so neither had steered round the other). On
// Novi Grad 5 of the 12 s rule's 8 traps were pile-ups like that, busy junctions by the enemy spawn with 10-16 links.
// Checks into a stuck spell (stuck at least 3 of them) at which it changes class anyway: half as long again as the
// stuck time (22.5 s for 15 s).
int LongSpellChecks(int killChecks) { return killChecks * 3 / 2; }

// The nearest other soldier within `radius` (feet within 1.5 m of height), or null.
const Actor* NearestBody(const std::vector<Actor>& actors, void* self, const float feet[3], float radius)
{
    const Actor* best = nullptr;
    for (const Actor& a : actors)
    {
        float d = Distance2D(feet, a.pos);
        if (a.player != self && d < radius && std::fabs(a.pos[1] - feet[1]) < 1.5f)
        {
            best = &a;
            radius = d;
        }
    }
    return best;
}

bool CheckStuck(State& s, const char* name, const float feet[3], float wantYaw, int moveState, int pose,
                const float* body, float dt)
{
    bool bodyNear = body != nullptr;
    s.stuckTimer -= dt;
    if (s.stuckTimer > 0.0f)
        return false;
    s.stuckTimer = 1.5f;
    bool stuck = Distance2D(feet, s.progressPos) < 0.6f;
    // A spell only ends 5 m (or 2 m of height) from where it began: shuffling about in a hole it can't climb out of
    // isn't getting free. Drumstick moved half a metre at a time for 6 minutes on Novi Grad, each move ending the
    // spell, so neither the trap nor the class change ever came.
    bool away = Distance2D(feet, s.stuckPos) >= 5.0f || std::fabs(feet[1] - s.stuckPos[1]) >= 2.0f;
    if (s.stuckStreak > 0)
        ++s.spellChecks;
    // Escalations by how long the spell has gone on, whichever check it lands on - as long as the spell goes on past
    // it: Dingus changed class on Novi Grad at the check that found it 7 m from the spot, free.
    bool goesOn = s.stuckStreak > 0 && (stuck || !away);
    if (goesOn && !bodyNear && s.stuckStreak >= 3 && s.spellChecks >= 8 && !s.spellTrapMarked)
    {
        s.spellTrapMarked = true;
        int links = nav::MarkTrap(s.stuckPos);
        if (g_settings.logEvents && links > 0)
            Log("brain %s: %.1f s without getting 5 m from (%.1f, %.1f, %.1f): marked as a trap (%d route links into it "
                "now cost the most)", name, s.spellChecks * 1.5f, s.stuckPos[0], s.stuckPos[1], s.stuckPos[2], links);
    }
    if (goesOn && !bodyNear && g_settings.stuckKillChecks && s.stuckStreak >= 3 &&
        s.spellChecks == LongSpellChecks(g_settings.stuckKillChecks))
        s.changeClass = true;
    if (stuck && bodyNear)
    {
        // Held up by another soldier: not the map's fault, so no streak, no route penalty, no trap. The steering in
        // Drive sidesteps it; half the time it also backs off a step or two first - two bots head-on in a corridor
        // would otherwise sidestep into its walls together for good.
        std::memcpy(s.progressPos, feet, sizeof(s.progressPos));
        if (Rand(s) < 0.5f)
        {
            s.detourYaw = WrapTwoPi(YawTo(feet[0] - body[0], feet[2] - body[2]) + Rand(s, -0.5f, 0.5f));
            s.detourTimer = Rand(s, 1.0f, 1.8f);
        }
        return false;
    }
    if (!stuck)
    {
        if (s.stuckStreak > 0 && !away)
        {
            std::memcpy(s.progressPos, feet, sizeof(s.progressPos));
            return false;
        }
        if (s.stuckStreak >= 3 && g_settings.logEvents && s.stuckLogs < 60)
        {
            ++s.stuckLogs;
            Log("brain %s: free again after %.1f s stuck at (%.1f, %.1f, %.1f)", name, s.spellChecks * 1.5f,
                s.stuckPos[0], s.stuckPos[1], s.stuckPos[2]);
        }
        s.stuckStreak = 0;
        s.spellChecks = 0;
        s.spellTrapMarked = false;
        std::memcpy(s.freePos, s.progressPos, sizeof(s.freePos));
        s.hasFreePos = true;
        std::memcpy(s.progressPos, feet, sizeof(s.progressPos));
        return false;
    }
    std::memcpy(s.progressPos, feet, sizeof(s.progressPos));
    if (++s.stuckStreak == 1)
    {
        std::memcpy(s.stuckPos, feet, sizeof(s.stuckPos));
        s.spellChecks = 1;
        s.spellTrapMarked = false;
    }
    const char* how = "the most open way";
    float failed = s.detourYaw;
    if (s.stuckStreak == 1)
        s.detourYaw = OpenestDirection(s, feet, wantYaw);
    else if (s.stuckStreak == 2 && s.hasFreePos && Distance2D(feet, s.freePos) > 0.5f)
    {
        s.detourYaw = YawTo(s.freePos[0] - feet[0], s.freePos[2] - feet[2]);
        how = "back the way it came";
    }
    else
    {
        s.detourYaw = OpenestDirection(s, feet, wantYaw, &failed);
        how = "another way";
    }
    bool jump = s.stuckStreak % 2 == 1 && !WaterAround(feet, 2.0f) && !WetAhead(feet, s.detourYaw, 3.0f);
    if (jump)
        s.jumpHold = 0.2f;
    s.detourTimer = Rand(s, 2.0f, 3.0f);
    if (s.stuckStreak == 6 && !s.spellTrapMarked)
    {
        s.spellTrapMarked = true;
        int links = nav::MarkTrap(s.stuckPos);
        if (g_settings.logEvents && links > 0)
            Log("brain %s: stuck 9 s of %.1f at (%.1f, %.1f, %.1f): marked as a trap (%d route links into it now cost "
                "the most)", name, s.spellChecks * 1.5f, s.stuckPos[0], s.stuckPos[1], s.stuckPos[2], links);
    }
    if (g_settings.stuckKillChecks && s.stuckStreak == g_settings.stuckKillChecks)
        s.changeClass = true;
    // Every check of the first 4.5 s, then every 6 s: the whole spell shows, however long.
    if (g_settings.logEvents && s.stuckLogs < 60 && (s.stuckStreak <= 3 || s.stuckStreak % 4 == 0))
    {
        ++s.stuckLogs;
        Log("brain %s: stuck at (%.1f, %.1f, %.1f) for %.1f s (stuck %d times; %s, %s); %s%s", name, feet[0], feet[1],
            feet[2], s.spellChecks * 1.5f, s.stuckStreak, eng::PoseName(pose), eng::StateName(moveState),
            jump ? "jumping, heading " : "heading ", how);
    }
    return true;
}

// Advances along the route and returns the point to walk to: the current waypoint, or the goal itself once the
// route is done (or when there is none). Presses jump approaching a waypoint whose link was a jump. It only moves on
// with its feet on the ground, and never past a waypoint more than a step above them, however close it is on the map:
// under a ledge the bot still has the climb to do (a hop at its foot used to count as being up there, so the next link
// got the blame for being stuck instead of the climb).
void FollowPath(State& s, const float feet[3], bool onGround, float target[3])
{
    std::memcpy(target, s.goal, sizeof(s.goal));
    while (onGround && s.pathIndex < s.path.size())
    {
        const nav::Waypoint& w = s.path[s.pathIndex];
        if (w.pos[1] - feet[1] > 1.4f)
            break;
        bool reached = Distance2D(feet, w.pos) < 1.4f && std::fabs(feet[1] - w.pos[1]) < 2.0f;
        // Already past it: closer to the next waypoint than this one is.
        bool passed = s.pathIndex + 1 < s.path.size() &&
                      Distance2D(feet, s.path[s.pathIndex + 1].pos) < Distance2D(w.pos, s.path[s.pathIndex + 1].pos);
        if (!reached && !passed)
            break;
        ++s.pathIndex;
    }
    if (s.pathIndex >= s.path.size())
        return;
    const nav::Waypoint& w = s.path[s.pathIndex];
    std::memcpy(target, w.pos, sizeof(w.pos));
    if (w.jump && s.jumpedAt != static_cast<int>(s.pathIndex) && Distance2D(feet, w.pos) < 2.5f)
    {
        s.jumpHold = 0.2f;
        s.jumpedAt = static_cast<int>(s.pathIndex);
    }
}

}  // namespace

void Init()
{
    g_settings.enabled = ReadInt(L"Enabled", 1) != 0;
    g_settings.skill = ReadInt(L"SkillPercent", 40) / 100.0f;
    g_settings.turnRate = ReadInt(L"TurnDegreesPerSecond", 240) * kPi / 180.0f;
    g_settings.aimErrorMeters = ReadInt(L"AimErrorCentimeters", 110) / 100.0f;
    float recoil = ReadInt(L"RecoilPercent", 40) / 100.0f;
    g_settings.recoilRate *= recoil;
    g_settings.recoilMax *= recoil;
    g_settings.reactionMin = ReadInt(L"ReactionMinMs", 350) / 1000.0f;
    g_settings.reactionMax = ReadInt(L"ReactionMaxMs", 850) / 1000.0f;
    g_settings.shootDistance = static_cast<float>(ReadInt(L"ShootDistance", 90));
    g_settings.sniperShootDistance = static_cast<float>(ReadInt(L"SniperShootDistance", 220));
    g_settings.aimForHead = ReadInt(L"AimForHead", 0) != 0;
    g_settings.logEvents = ReadInt(L"LogEvents", 1) != 0;
    g_settings.halfFov = ReadInt(L"FovDegrees", 120) * 0.5f * kPi / 180.0f;
    g_settings.grenadeChance = ReadInt(L"GrenadePercent", 35) / 100.0f;
    g_settings.spawnGrace = ReadInt(L"SpawnGraceMs", 2000) / 1000.0f;
    g_settings.sniperSettle = ReadInt(L"SniperSettleMs", 800) / 1000.0f;
    g_settings.aimSettle = ReadInt(L"AimSettleMs", 900) / 1000.0f;
    g_settings.noticeTime = ReadInt(L"NoticeMs", 450) / 1000.0f;
    g_settings.hearGunfire = static_cast<float>(ReadInt(L"HearGunfireMeters", 30));
    g_settings.hearSprint = static_cast<float>(ReadInt(L"HearSprintMeters", 10));
    g_settings.hearRun = static_cast<float>(ReadInt(L"HearRunMeters", 5));
    g_settings.hearWalk = static_cast<float>(ReadInt(L"HearWalkMeters", 2));
    g_settings.botAimError = ReadInt(L"BotTargetAimErrorPercent", 100) / 100.0f;
    g_settings.flashMeters = static_cast<float>(ReadInt(L"FlashbangMeters", 15));
    g_settings.flashSeconds = ReadInt(L"FlashbangBlindMs", 5000) / 1000.0f;
    g_settings.smokeBlocksSight = ReadInt(L"SmokeBlocksSight", 1) != 0;
    Log("brain: flashbangs blind bots that see them within %.0f m (up to %.1f s); smoke %s", g_settings.flashMeters,
        g_settings.flashSeconds, g_settings.smokeBlocksSight ? "blocks their sight" : "is ignored");
    int stuckKill = ReadInt(L"StuckChangeClassSeconds", 15);
    g_settings.stuckKillChecks = stuckKill > 0 ? (stuckKill * 2 + 2) / 3 : 0;  // whole 1.5 s checks, rounded up
    if (g_settings.stuckKillChecks)
        Log("brain: a bot stuck %.1f s - or %.1f s of a stuck spell without getting 5 m away - changes class to get out "
            "(dies the way a player's class change does; respawns as itself)",
            g_settings.stuckKillChecks * 1.5f, LongSpellChecks(g_settings.stuckKillChecks) * 1.5f);
    Log("brain: bots steer round a soldier in their way (within 2 m); held up by one (within 1.5 m) isn't stuck - no "
        "route penalty, trap or class change");
    Log("brain: %s, skill %.2f, turn %.0f deg/s, aim error %.2f m (x%.2f against bots), recoil %.0f%%, notice %.2f s, "
        "reaction %.2f-%.2f s, hearing gunfire %.0f / sprint %.0f / run %.0f / walk %.0f m, range %.0f m (sniper %.0f)",
        g_settings.enabled ? "on" : "off", g_settings.skill, g_settings.turnRate * 180.0f / kPi,
        g_settings.aimErrorMeters, g_settings.botAimError, recoil * 100.0f, g_settings.noticeTime,
        g_settings.reactionMin, g_settings.reactionMax, g_settings.hearGunfire, g_settings.hearSprint,
        g_settings.hearRun, g_settings.hearWalk, g_settings.shootDistance, g_settings.sniperShootDistance);
}

bool Enabled() { return g_settings.enabled; }

bool InCombat(const State& s, float window) { return (s.target && s.sinceSeen < window) || s.sinceFired < window; }

void SetObjectives(const std::vector<Objective>& objectives)
{
    g_objectives = objectives;
}

void TakeGoalCounts(int counts[kGoalKinds])
{
    for (int k = 0; k < kGoalKinds; ++k)
    {
        counts[k] = g_goalCounts[k];
        g_goalCounts[k] = 0;
    }
}
void SetEnabled(bool enabled) { g_settings.enabled = enabled; }

void Idle(void* input)
{
    State still;
    still.yaw = *reinterpret_cast<float*>(static_cast<char*>(input) + eng::kInput_Yaw);  // keep facing
    Write(input, still, Output{});
}

void Update(State& s, const char* name, void* player, void* soldier, void* input, int team, int kitType,
            const std::vector<Actor>& actors, float dt)
{
    const float* t = nullptr;
    if (!eng::GetSoldierTransform(soldier, &t) || !t)
        return;
    float feet[3] = {t[12], t[13], t[14]};
    const Actor* self = FindActor(actors, player);
    int pose = self ? PoseIndex(self->pose) : eng::kPoseStand;
    int moveState = self ? self->state : eng::kStateOnGround;
    float eye[3] = {feet[0], feet[1] + kEyeHeight[pose], feet[2]};
    if (!s.aimInitialised)
    {
        s.rng = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(player)) ^ GetTickCount();
        s.yaw = YawTo(t[8], t[10]);  // face where the soldier spawned facing (forward row of its transform)
        s.pitch = 0.0f;
        std::memcpy(s.progressPos, feet, sizeof(s.progressPos));
        s.stuckTimer = 1.5f;  // first check after it had a chance to move (it used to call itself stuck on tick 1)
        s.aimInitialised = true;
    }

    Output o;
    s.sinceFired += dt;

    // Flashbangs since the last tick (a fresh life only counts the ones from now on).
    int lastFlash = grenades::LastFlashId();
    if (s.flashSeen < 0)
        s.flashSeen = lastFlash;
    if (lastFlash > s.flashSeen)
    {
        for (const grenades::Flash& f : grenades::RecentFlashes())
        {
            if (f.id <= s.flashSeen)
                continue;
            float distance = 0.0f, off = 0.0f;
            float blind = FlashSeconds(eye, s.yaw, f.pos, &distance, &off);
            if (blind >= 0.5f && blind > s.blindTimer)
            {
                s.blindTimer = blind;
                if (g_settings.logEvents)
                    Log("brain %s: flashbanged for %.1f s (%.0f m away, looking %.0f deg off it)", name, blind,
                        distance, off * 180.0f / kPi);
            }
        }
        s.flashSeen = lastFlash;
    }

    // Posture. Bots never crouch, go prone or swim on purpose. Swimming heads back to the last dry spot (or the
    // nearest dry nav node); crouched or prone on the ground presses jump, which stands a soldier up (0x7EA930: jump
    // while prone -> setPose(stand) instead of jumping). Vaults (animation-controlled) crouch on their own: left alone.
    if (moveState == eng::kStateSwimming)
    {
        // Dropped in from higher ground (MP_03's bay: routes along a strip at the water's edge, 2 m up a slippery
        // slope, put three bots in the sea in one round): the route nodes on the edge right above where it went in -
        // else around where it last stood dry - become a trap, so routes stop going there.
        if (s.swimTime == 0.0f && s.hasDryPos && s.dryPos[1] - feet[1] > 1.0f && Distance2D(s.dryPos, feet) < 8.0f)
        {
            float edge[3] = {feet[0], feet[1] + 2.0f, feet[2]};
            int links = nav::MarkTrap(edge);
            if (!links)
                links = nav::MarkTrap(s.dryPos);
            if (g_settings.logEvents && links > 0)
                Log("brain %s: fell into the water at (%.1f, %.1f, %.1f) from (%.1f, %.1f, %.1f): marked as a trap (%d "
                    "route links into it now cost the most)", name, feet[0], feet[1], feet[2], s.dryPos[0],
                    s.dryPos[1], s.dryPos[2], links);
        }
        if (s.swimTime == 0.0f && g_settings.logEvents && s.postureLogs < 30)
        {
            ++s.postureLogs;
            Log("brain %s: swimming at (%.1f, %.1f, %.1f) (water probe: %.1f m deep), %.1f s after spawning; heading "
                "back to dry ground", name, feet[0], feet[1], feet[2], eng::WaterDepth(feet),
                self ? self->spawnAge : -1.0f);
        }
        s.swimTime += dt;
    }
    else
    {
        s.swimTime = 0.0f;
        s.dryTimer -= dt;
        if (moveState == eng::kStateOnGround && pose == eng::kPoseStand && s.dryTimer <= 0.0f)
        {
            s.dryTimer = 1.0f;
            if (eng::WaterDepth(feet) < 0.2f)
            {
                std::memcpy(s.dryPos, feet, sizeof(s.dryPos));
                s.hasDryPos = true;
            }
        }
    }
    bool standUp = false;
    if (pose != eng::kPoseStand && moveState == eng::kStateOnGround)
    {
        if (s.offPoseTime < 0.3f && s.offPoseTime + dt >= 0.3f && g_settings.logEvents && s.postureLogs < 30)
        {
            ++s.postureLogs;
            Log("brain %s: found itself %s at (%.1f, %.1f, %.1f) (%s); standing up", name, eng::PoseName(pose),
                feet[0], feet[1], feet[2], eng::StateName(moveState));
        }
        s.offPoseTime += dt;
        // A 0.12 s press 0.3 s in, then every 0.8 s until it stands.
        standUp = s.offPoseTime > 0.3f && std::fmod(s.offPoseTime - 0.3f, 0.8f) < 0.12f;
    }
    else if (s.offPoseTime > 0.0f)
    {
        if (s.offPoseTime > 2.0f && g_settings.logEvents && s.postureLogs < 30)
        {
            ++s.postureLogs;
            Log("brain %s: standing again after %.1f s", name, s.offPoseTime);
        }
        s.offPoseTime = 0.0f;
    }

    // In water a bot can't fight: it drops its target and gets out first.
    bool swimming = s.swimTime >= 0.5f;
    // Flashbanged: blind and deaf - it loses its target and everyone it had picked out, holds its fire and stumbles
    // back from where it was looking until it can see again.
    if (s.blindTimer > 0.0f && !swimming)
    {
        s.blindTimer -= dt;
        s.target = nullptr;
        s.awareness.clear();
        s.fireOn = false;
        MoveToward(s, WrapTwoPi(s.yaw + kPi), 0.5f, o);
        Write(input, s, o);
        return;
    }
    if (!swimming)
        Perceive(s, name, team, kitType, feet, eye, actors, dt);
    bool engaged = s.target && s.sinceSeen < 2.5f && !swimming;
    if (s.target && !engaged)
    {
        // Lost sight for a while: go to where it was last seen.
        if (g_settings.logEvents)
            Log("brain %s: lost sight of %p; moving to last known position", name, s.target);
        std::memcpy(s.goal, s.targetPos, sizeof(s.goal));
        s.hasGoal = eng::WaterDepth(s.goal) < 0.4f;  // not after it into the bay
        s.goalTimer = 8.0f;
        s.replanTimer = 0.0f;  // route there
        s.target = nullptr;
    }

    if (engaged)
    {
        // Aim: chest (or head) for the target's pose, with skill- and distance-scaled error re-rolled a few times a
        // second.
        const Actor* targetActor = FindActor(actors, s.target);
        int targetPose = targetActor ? PoseIndex(targetActor->pose) : eng::kPoseStand;
        float height = s.headAim ? kHeadHeight[targetPose] : kChestHeight[targetPose];
        float dx = s.targetPos[0] - eye[0];
        float dy = s.targetPos[1] + height - eye[1];
        float dz = s.targetPos[2] - eye[2];
        float flat = std::sqrt(dx * dx + dz * dz);
        float distance = std::sqrt(flat * flat + dy * dy);
        // Aim error = fun-bots aim worsening plus what recoil and spread would add for a human (bot shots in MOHW
        // don't climb on their own): a sway drifting toward offsets re-rolled every 0.2-0.4 s, per-tick jitter while
        // firing so a burst sprays instead of stacking on one point, and recoil climbing while the trigger is held.
        // Miss distance grows with range but stays small up close: AimErrorCentimeters x d / (d + 10) metres, i.e.
        // ~0.2x at 3 m, 0.5x at 10 m, 0.75x at 30 m. (A fixed miss distance, as before, was a huge angle up close:
        // bots at 3 m sprayed past each other.)
        float spread = g_settings.aimErrorMeters * (1.5f - g_settings.skill) / (distance + 10.0f);
        if (s.moveWhileShooting && distance < 30.0f)
            spread *= 1.5f;  // shooting on the move
        // Against another bot: BotTargetAimErrorPercent (100 = as against you). It was 60 while bot-vs-bot hits went
        // missing - the cause turned out to be the server dropping them (see combatlog BotHitsOnBotsByServer).
        if (targetActor && targetActor->isBot)
            spread *= g_settings.botAimError;
        // Just swung onto a new target: up to 3x the error, fading over AimSettleMs (the first shots after a turn
        // go wide, as a human's do).
        spread *= 1.0f + 2.0f * s.settle;
        if (g_settings.aimSettle > 0.0f)
            s.settle = std::fmax(s.settle - dt / g_settings.aimSettle, 0.0f);
        s.aimErrTimer -= dt;
        if (s.aimErrTimer <= 0.0f)
        {
            s.aimErrTimer = Rand(s, 0.2f, 0.4f);
            s.swayTargetYaw = Rand(s, -1.0f, 1.0f) * spread;
            s.swayTargetPitch = Rand(s, -1.0f, 1.0f) * spread;
        }
        float blend = dt * 6.0f < 1.0f ? dt * 6.0f : 1.0f;
        s.aimErrYaw += (s.swayTargetYaw - s.aimErrYaw) * blend;
        s.aimErrPitch += (s.swayTargetPitch - s.aimErrPitch) * blend;
        if (s.fireOn)
            s.recoil = std::fmin(s.recoil + dt * g_settings.recoilRate, g_settings.recoilMax);
        else
            s.recoil = std::fmax(s.recoil - dt * g_settings.recoilRate * 2.0f, 0.0f);
        float uncorrected = s.recoil * (1.0f - 0.6f * g_settings.skill);  // better bots pull down more
        float jitterYaw = s.fireOn ? Rand(s, -0.5f, 0.5f) * spread + Rand(s, -0.3f, 0.3f) * uncorrected : 0.0f;
        float jitterPitch = s.fireOn ? Rand(s, -0.5f, 0.5f) * spread : 0.0f;
        float aimYaw = WrapTwoPi(YawTo(dx, dz) + s.aimErrYaw);
        float wantYaw = WrapTwoPi(aimYaw + jitterYaw);
        float wantPitch = std::atan2(dy, flat) + s.aimErrPitch + jitterPitch + uncorrected;
        TurnToward(s, wantYaw, wantPitch, dt);
        const float aimedAt[3] = {s.targetPos[0], s.targetPos[1] + height, s.targetPos[2]};
        if (hardcore::FriendlyFire() && FriendInLine(player, team, eye, aimedAt, actors))
        {
            s.fireOn = false;  // the same reaction and aim go on; it fires as soon as the line is clear
            s.burstTimer = 0.0f;
        }
        else
            Fire(s, kitType, distance, AngleDiff(s.yaw, aimYaw), dt, o);

        // Grenade at an enemy who just ducked into cover 8-25 m away (fun-bots throws within 25 m; its pitch table
        // only covers that range). One roll per time the target disappears.
        s.grenadeCooldown -= dt;
        if (s.sinceSeen < 0.3f)
            s.grenadeRolled = false;
        else if (s.sinceSeen > 0.5f && s.sinceSeen < 2.0f && !s.grenadeRolled)
        {
            s.grenadeRolled = true;
            if (s.grenadeCooldown <= 0.0f && flat >= 8.0f && flat <= 25.0f && Rand(s) < g_settings.grenadeChance &&
                !(hardcore::FriendlyFire() && FriendNear(player, team, s.targetPos, 7.0f, actors)))
            {
                s.grenadeHold = 0.6f;
                if (g_settings.logEvents)
                    Log("brain %s: grenade at the enemy's cover, %.0f m", name, flat);
            }
        }
        if (s.grenadeHold > 0.0f)
        {
            TurnToward(s, YawTo(dx, dz), GrenadePitch(flat), dt);  // turn and lob, no aim error
            o.fire = false;
            o.throwGrenade = s.grenadeHold < 0.25f;  // press once lined up
            s.grenadeHold -= dt;
            if (s.grenadeHold <= 0.0f)
                s.grenadeCooldown = Rand(s, 15.0f, 25.0f);
        }

        // Move while shooting (fun-bots StateAttacking: re-decide every 2-4 s whether to stop or strafe).
        s.attackMoveTimer -= dt;
        if (s.attackMoveTimer <= 0.0f)
        {
            s.attackMoveTimer = Rand(s, 2.0f, 4.0f);
            s.moveWhileShooting = kitType != kKitSniper && Rand(s) > 0.4f;
        }
        if (s.moveWhileShooting && flat < 30.0f)
        {
            s.strafeTimer -= dt;
            if (s.strafeTimer <= 0.0f)
            {
                s.strafeTimer = Rand(s, 0.7f, 1.6f);
                s.strafeDir = Rand(s) < 0.5f ? -1.0f : 1.0f;
            }
            o.strafe = s.strafeDir;
        }
        else if (flat > 50.0f && kitType != kKitSniper)
            o.throttle = 0.6f;  // close in on distant targets
        // Aim down sights (as a human does; hip fire's spread is several times wider), except point blank and while
        // lining up a grenade. Snipers always scope in.
        o.zoom = s.grenadeHold <= 0.0f && (kitType == kKitSniper || flat > 6.0f);
        if (s.jumpHold > 0.0f)
            s.jumpHold = 0.0f;  // no jumping mid-fight
        s.hasGoal = false;
    }
    else
    {
        s.fireOn = false;
        // Reload in quiet moments.
        if (s.triggerTime > 2.5f)
        {
            s.triggerTime = 0.0f;
            s.reloadHold = 0.15f;
        }

        s.goalTimer -= dt;
        bool holding = false;
        bool still = false;  // standing at its spot on the flag, watching a way in
        if (s.hasGoal && s.goalKind == kGoalObjective && s.goalTimer > 0.0f)
        {
            if (!ObjectiveWanted(s.objective, team, actors))
                s.hasGoal = false;  // taken (and nobody threatening it): something else to do
            else
            {
                holding = true;
                const Objective& ob = g_objectives[s.objective];
                if (s.heldObjective != s.objective)
                {
                    s.heldObjective = s.objective;
                    s.heldFor = 0.0f;
                }
                if (ob.owner == team)
                    s.heldFor = 0.0f;  // theirs (it's defending): never goes stale
                else if (Distance2D(feet, ob.pos) < ob.radius && std::fabs(feet[1] - ob.pos[1]) < 4.0f)
                    s.heldFor += dt;
                // There: stand and watch (it used to pick a new spot every time it reached one, so it ran in circles);
                // every 8-18 s, half the time, it shifts to another spot inside.
                if (Distance2D(feet, s.goal) < 1.2f)
                {
                    if (!s.atSpot)
                    {
                        s.atSpot = true;
                        s.spotTimer = Rand(s, 8.0f, 18.0f);
                        s.watchTimer = 0.0f;
                    }
                    s.spotTimer -= dt;
                    if (s.spotTimer <= 0.0f && Rand(s) < 0.5f)
                        ObjectiveSpot(s, ob, s.goal);
                    else
                    {
                        if (s.spotTimer <= 0.0f)
                            s.spotTimer = Rand(s, 8.0f, 18.0f);
                        still = true;
                    }
                }
            }
        }
        if (!holding && (!s.hasGoal || s.goalTimer <= 0.0f || Distance2D(feet, s.goal) < 3.0f))
        {
            PickGoal(s, player, team, feet, actors);
            s.replanTimer = 0.0f;
        }

        // Route over the learned graph (re-planned every 5 s as enemies move); straight at the goal without one.
        s.replanTimer -= dt;
        if (s.replanTimer <= 0.0f)
        {
            s.replanTimer = 5.0f;
            int failure = 0;
            if (!nav::FindPath(feet, s.goal, s.path, &failure))
            {
                s.path.clear();
                if (g_settings.logEvents && nav::NodeCount() > 0 && s.routeLogs < 8)
                {
                    ++s.routeLogs;
                    Log("brain %s: no route from (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f): %s", name, feet[0], feet[1],
                        feet[2], s.goal[0], s.goal[1], s.goal[2],
                        failure == 1 ? "nobody has walked near me" : failure == 2 ? "nobody has walked near the goal"
                                                                   : "the two areas aren't connected yet");
                }
            }
            s.pathIndex = 0;
            s.jumpedAt = -1;
        }
        float target[3];
        FollowPath(s, feet, moveState == eng::kStateOnGround, target);
        bool onPath = s.pathIndex < s.path.size();
        bool swimmingOut = false;
        if (s.swimTime > 0.5f)
        {
            // An exit: where it last stood dry if it can climb back up there, else the nearest dry nav node with a
            // bank low enough to climb (within 1.2 m of the water, up to 80 m away), else any dry node within 60 m -
            // skipping exits that already failed. The ledge it fell from (2-6 m up) was a dead end every time.
            if (!s.hasSwimExit)
            {
                constexpr float kClimb = 1.2f;
                bool dryOk = s.hasDryPos && s.dryPos[1] - feet[1] <= kClimb;
                for (int k = 0; k < s.failedExitCount && dryOk; ++k)
                    dryOk = Distance2D(s.dryPos, s.failedExits[k]) >= 6.0f;
                if (dryOk)
                    std::memcpy(s.swimExit, s.dryPos, sizeof(s.swimExit));
                s.hasSwimExit = dryOk ||
                                nav::NearestNodeAvoiding(feet, 80.0f, s.failedExits, s.failedExitCount, 6.0f,
                                                         s.swimExit, kClimb) ||
                                nav::NearestNodeAvoiding(feet, 60.0f, s.failedExits, s.failedExitCount, 6.0f,
                                                         s.swimExit);
                s.swimCheckTimer = 3.0f;
                s.swimCheckDist = s.hasSwimExit ? Distance2D(feet, s.swimExit) : 0.0f;
            }
            else
            {
                // No metre gained in 3 s: this exit is blocked (wall, ledge); remember it and pick another.
                s.swimCheckTimer -= dt;
                if (s.swimCheckTimer <= 0.0f)
                {
                    float d = Distance2D(feet, s.swimExit);
                    if (d > s.swimCheckDist - 1.0f)
                    {
                        if (s.failedExitCount == 4)
                            s.failedExitCount = 0;  // tried four: start over
                        std::memcpy(s.failedExits[s.failedExitCount++], s.swimExit, sizeof(s.swimExit));
                        s.hasSwimExit = false;
                        if (g_settings.logEvents && s.postureLogs < 30)
                        {
                            ++s.postureLogs;
                            Log("brain %s: can't get out of the water at (%.1f, %.1f, %.1f); trying another way out",
                                name, s.swimExit[0], s.swimExit[1], s.swimExit[2]);
                        }
                    }
                    s.swimCheckTimer = 3.0f;
                    s.swimCheckDist = d;
                }
            }
            if (s.hasSwimExit)
            {
                swimmingOut = true;
                std::memcpy(target, s.swimExit, sizeof(target));
                onPath = false;
                // Near the bank: jump now and then to clamber up a ledge.
                if (Distance2D(feet, s.swimExit) < 4.0f && std::fmod(s.swimTime, 1.0f) < dt)
                    s.jumpHold = 0.2f;
            }
        }
        else if (s.hasSwimExit || s.failedExitCount)
        {
            s.hasSwimExit = false;
            s.failedExitCount = 0;
        }

        float goalDist = Distance2D(feet, s.goal);
        float wantYaw = YawTo(target[0] - feet[0], target[2] - feet[2]);
        // Bodies block each other but not the probes, so steer round a soldier in the way: within 2 m and ahead within
        // 75 degrees, veer 1 rad off its side for 0.6 s - dead ahead always the same turn, so two bots meeting head-on
        // pass each other. A third of Novi Grad's stuck time was bots stuck within 4 m of each other at once.
        const Actor* body = swimmingOut ? nullptr : NearestBody(actors, player, feet, 2.0f);
        if (body && s.detourTimer <= 0.0f)
        {
            float off = AngleDiff(wantYaw, YawTo(body->pos[0] - feet[0], body->pos[2] - feet[2]));
            if (std::fabs(off) < 1.3f)
            {
                s.detourYaw = WrapTwoPi(wantYaw + ((std::fabs(off) < 0.25f || off < 0.0f) ? 1.0f : -1.0f));
                s.detourTimer = 0.6f;
            }
        }
        float moveYaw = swimmingOut ? wantYaw : AvoidObstacles(s, feet, wantYaw, onPath, dt);
        still = still && !swimmingOut;
        // Look where it walks - or toward a noise it heard, still walking the same way. Standing on a flag: down the
        // way in it's watching, a new one every 2.5-5 s.
        s.lookTimer -= dt;
        if (s.lookTimer > 0.0f)
            TurnToward(s, s.lookYaw, 0.0f, dt);
        else if (still)
        {
            s.watchTimer -= dt;
            if (s.watchTimer <= 0.0f)
            {
                s.watchYaw = WatchYaw(s, s.objective, team, eye);
                s.watchTimer = Rand(s, 2.5f, 5.0f);
            }
            TurnToward(s, s.watchYaw, 0.0f, dt);
        }
        else
            TurnToward(s, moveYaw, 0.0f, dt);
        if (!still)
            MoveToward(s, moveYaw, 1.0f, o);
        o.sprint = !still && goalDist > 20.0f && s.detourTimer <= 0.0f && s.lookTimer <= 0.0f && !swimmingOut;
        if (still)
            std::memcpy(s.progressPos, feet, sizeof(s.progressPos));  // standing on purpose isn't being stuck
        bool stuck = !still && CheckStuck(s, name, feet, wantYaw, moveState, pose,
                                          body && Distance2D(feet, body->pos) < 1.5f ? body->pos : nullptr, dt);
        if (s.changeClass)
        {
            s.changeClass = false;
            // Every way out has failed (the Novi Grad nook held Doug 5 minutes): what a player does - change class,
            // which kills you (the server's answer to the deploy screen's NetworkSuicideMessage). It respawns as
            // itself, with its kit, 5 s later; the spot is already marked as a trap.
            bool killed = eng::KillSoldier(soldier);
            if (g_settings.logEvents)
                Log("brain %s: stuck %.1f s of %.1f at (%.1f, %.1f, %.1f): %s", name, s.stuckStreak * 1.5f,
                    s.spellChecks * 1.5f, feet[0], feet[1], feet[2], killed ? "changing class to get out (dies, respawns as itself)"
                                    : "changing class FAILED (left stuck)");
        }
        if (stuck && onPath)
        {
            // Stuck on a learned link: make it costlier.
            if (s.pathIndex > 0)
                nav::Penalize(s.path[s.pathIndex - 1].node, s.path[s.pathIndex].node);
            if (s.pathIndex > 0 && s.stuckStreak < 3)
            {
                s.replanTimer = 0.0f;  // route again
                s.detourTimer = 0.0f;  // the new route decides
            }
            else
            {
                // Can't reach the route's first node (e.g. it's behind a wall), or new routes haven't freed it in
                // 4.5 s - every one walks straight at its next waypoint, which from a nook can be straight into the
                // wall (Doug spent 5 minutes under a ledge on Novi Grad): roam with detours for a while.
                if (g_settings.logEvents && s.stuckStreak == 3)
                    Log("brain %s: routes aren't freeing it; leaving the route for a while", name);
                s.path.clear();
                s.replanTimer = 4.0f;
            }
        }
    }

    if (s.jumpHold > 0.0f)
    {
        o.jump = true;
        s.jumpHold -= dt;
    }
    if (standUp)
        o.jump = true;  // also mid-fight: a bot lying down shoots off to the side of where it points
    if (s.reloadHold > 0.0f)
    {
        o.reload = true;
        s.reloadHold -= dt;
    }

    // One-off convention check: after aiming a while, does the soldier face where the input says?
    s.aimCheckTimer += dt;
    if (!s.loggedAimCheck && s.aimCheckTimer > 3.0f && g_settings.logEvents)
    {
        s.loggedAimCheck = true;
        Log("brain %s: aim check - input yaw %.2f pitch %.2f; soldier forward (%.2f, %.2f, %.2f) = yaw %.2f; "
            "throttle %.2f strafe %.2f",
            name, s.yaw, s.pitch, t[8], t[9], t[10], YawTo(t[8], t[10]), o.throttle, o.strafe);
    }

    Write(input, s, o);
}

}  // namespace brain
