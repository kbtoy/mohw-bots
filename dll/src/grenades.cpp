#include "grenades.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine.h"
#include "log.h"

namespace grenades {
namespace {

constexpr uint32_t kCategoryCollision = 0xDB60E01E;
constexpr uint32_t kExplosionSpawn = 0x0376E37B;  // ServerCollisionExplosionSpawnMessage (0x50 bytes)
constexpr int kMessage_Explosion = 0x28;
constexpr int kMessage_Position = 0x30;
constexpr int kMessage_BlastRadius = 0x40;
constexpr int kMessage_BlastDamage = 0x44;
constexpr int kEntity_Data = 0x14;
constexpr int kExplosionData_HasStunEffect = 0xC4;

// Live server smoke screens: every ServerSmokeScreenEntity (vtable 0x27B7FC0, 0xA0 bytes) adds itself to a global
// eastl::vector (begin 0x2AF1730, end 0x2AF1734) in its ctor (0xB3DFD0) and leaves it in its dtor (0xB3A300); the
// teammate-in-your-smoke score check (0xEAA160) walks it the same way. +0x14 its SmokeScreenEntityData, +0x94 age in
// seconds, +0x98 active, +0x9C the thrower (weak ref).
constexpr uintptr_t kSmokeList_Begin = 0x2AF1730;
constexpr uintptr_t kSmokeList_End = 0x2AF1734;
constexpr int kSmoke_Age = 0x94;
constexpr int kSmoke_Active = 0x98;
constexpr int kSmokeData_ShrinkDelay = 0x70;
constexpr int kSmokeData_LifeTime = 0x74;
constexpr int kSmokeData_Radius = 0x7C;
constexpr int kSmokeData_RadiusTimeFallOff = 0x81;
// The entity's transform, as 0xEAA160 reads it: [e+0x1C], or with flag bit 20 of [e+8] the block for its first
// component (+0x10 + byte[+6] x 0x50); the position is at +0x30.
constexpr int kEntity_Flags = 0x08;
constexpr int kEntity_TransformBlock = 0x1C;
constexpr int kTransform_Position = 0x30;

constexpr uint64_t kFlashMemoryMs = 2000;
constexpr float kSmokeCentreHeight = 1.0f;  // round a ground burst: chest-high sight lines go through the middle
constexpr size_t kMaxSmokes = 64;

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Flash> g_flashes;
int g_lastFlashId = 0;
int g_loggedExplosions = 0;

// Smoke screens seen so far (server thread only): logged as they come up and as they clear, with how many bot sight
// lines each one hid (a bot looks every 0.15-0.25 s, so one bot staring into it adds about 5 a second).
struct Tracked
{
    void* smoke = nullptr;
    float pos[3] = {};
    uint64_t upAt = 0;
    int hidden = 0;
    bool gone = false;
};
std::vector<Tracked> g_tracked;

Tracked* TrackedFor(void* smoke)
{
    for (Tracked& t : g_tracked)
        if (t.smoke == smoke)
            return &t;
    return nullptr;
}

void LogGone(const Tracked& t)
{
    Log("grenades: smoke screen at (%.1f, %.1f, %.1f) cleared after %.0f s: it hid %d bot sight lines", t.pos[0],
        t.pos[1], t.pos[2], (GetTickCount64() - t.upAt) / 1000.0, t.hidden);
}

// The cloud's centre and its radius right now - 0xB2A070's sum: the data's radius, shrinking linearly to nothing
// between ShrinkDelay and LifeTime when it falls off with time. False when it isn't up.
bool SmokeNow(void* smoke, float centre[3], float* radius)
{
    uint8_t active = 0;
    void* data = nullptr;
    float r = 0.0f, life = 0.0f, shrink = 0.0f, age = 0.0f;
    uint8_t fallOff = 0;
    if (!eng::ReadByte(eng::At(smoke, kSmoke_Active), &active) || !active ||
        !eng::ReadPtr(eng::At(smoke, kEntity_Data), &data) || !data ||
        !eng::ReadFloat(eng::At(data, kSmokeData_Radius), &r) || !(r > 0.0f && r < 100.0f))
        return false;
    eng::ReadFloat(eng::At(data, kSmokeData_LifeTime), &life);
    eng::ReadFloat(eng::At(data, kSmokeData_ShrinkDelay), &shrink);
    eng::ReadByte(eng::At(data, kSmokeData_RadiusTimeFallOff), &fallOff);
    eng::ReadFloat(eng::At(smoke, kSmoke_Age), &age);
    if (fallOff && life > shrink && age > shrink)
        r *= std::fmax(0.0f, 1.0f - (age - shrink) / (life - shrink));
    uint32_t flags = 0;
    void* block = nullptr;
    if (r <= 0.0f || !eng::ReadU32(eng::At(smoke, kEntity_Flags), &flags) ||
        !eng::ReadPtr(eng::At(smoke, kEntity_TransformBlock), &block) || !block)
        return false;
    if ((flags >> 20) & 1)
    {
        uint8_t index = 0;
        if (!eng::ReadByte(eng::At(block, 6), &index))
            return false;
        block = static_cast<char*>(block) + 0x10 + index * 0x50;
    }
    if (!eng::ReadBytes(eng::At(block, kTransform_Position), centre, 3 * sizeof(float)))
        return false;
    centre[1] += kSmokeCentreHeight;
    *radius = r;
    return true;
}

// The live smoke screens (at most kMaxSmokes).
size_t Smokes(void* out[kMaxSmokes])
{
    void* begin = nullptr;
    void* end = nullptr;
    if (!eng::ReadPtr(reinterpret_cast<const void*>(kSmokeList_Begin), &begin) ||
        !eng::ReadPtr(reinterpret_cast<const void*>(kSmokeList_End), &end) || !begin || end < begin)
        return 0;
    size_t count = std::min<size_t>((static_cast<char*>(end) - static_cast<char*>(begin)) / sizeof(void*), kMaxSmokes);
    size_t n = 0;
    for (size_t i = 0; i < count; ++i)
        if (eng::ReadPtr(static_cast<void**>(begin) + i, &out[n]) && out[n])
            ++n;
    return n;
}

// Squared distance from c to the segment a-b.
float SegmentDistanceSq(const float a[3], const float b[3], const float c[3])
{
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float len = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = len > 0.0f ? (ac[0] * ab[0] + ac[1] * ab[1] + ac[2] * ab[2]) / len : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float d[3] = {ac[0] - ab[0] * t, ac[1] - ab[1] * t, ac[2] - ab[2] * t};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

}  // namespace

void OnMessage(const void* message)
{
    const uint32_t* header = static_cast<const uint32_t*>(message);
    if (header[1] != kCategoryCollision || header[2] != kExplosionSpawn)
        return;
    void* explosion = nullptr;
    void* data = nullptr;
    float pos[3] = {};
    float radius = 0.0f, damage = 0.0f;
    if (!eng::ReadPtr(eng::At(message, kMessage_Explosion), &explosion) || !explosion ||
        !eng::ReadPtr(eng::At(explosion, kEntity_Data), &data) || !data ||
        !eng::ReadBytes(eng::At(message, kMessage_Position), pos, sizeof(pos)))
        return;
    eng::ReadFloat(eng::At(message, kMessage_BlastRadius), &radius);
    eng::ReadFloat(eng::At(message, kMessage_BlastDamage), &damage);
    uint8_t stun = 0;
    eng::ReadByte(eng::At(data, kExplosionData_HasStunEffect), &stun);
    uint64_t now = GetTickCount64();

    if (g_loggedExplosions < 12)
    {
        ++g_loggedExplosions;
        Log("grenades: explosion at (%.1f, %.1f, %.1f): blast %.1f m / %.0f damage, stun %d (first 12 logged)", pos[0],
            pos[1], pos[2], radius, damage, stun);
    }
    if (stun)
    {
        AcquireSRWLockExclusive(&g_lock);
        Flash f;
        f.id = ++g_lastFlashId;
        std::memcpy(f.pos, pos, sizeof(f.pos));
        f.at = now;
        g_flashes.push_back(f);
        while (!g_flashes.empty() && now - g_flashes.front().at > kFlashMemoryMs)
            g_flashes.erase(g_flashes.begin());
        ReleaseSRWLockExclusive(&g_lock);
        Log("grenades: flashbang at (%.1f, %.1f, %.1f)", pos[0], pos[1], pos[2]);
    }
}

std::vector<Flash> RecentFlashes()
{
    AcquireSRWLockShared(&g_lock);
    std::vector<Flash> out = g_flashes;
    ReleaseSRWLockShared(&g_lock);
    return out;
}

int LastFlashId()
{
    AcquireSRWLockShared(&g_lock);
    int id = g_lastFlashId;
    ReleaseSRWLockShared(&g_lock);
    return id;
}

bool SmokeBlocks(const float from[3], const float to[3])
{
    void* smokes[kMaxSmokes];
    size_t n = Smokes(smokes);
    for (size_t i = 0; i < n; ++i)
    {
        float centre[3] = {}, r = 0.0f;
        if (SmokeNow(smokes[i], centre, &r) && SegmentDistanceSq(from, to, centre) < r * r)
        {
            if (Tracked* t = TrackedFor(smokes[i]))
                ++t->hidden;
            return true;
        }
    }
    return false;
}

void ServerTick()
{
    void* smokes[kMaxSmokes];
    size_t n = Smokes(smokes);
    for (size_t i = 0; i < g_tracked.size();)
        if (std::find(smokes, smokes + n, g_tracked[i].smoke) == smokes + n)
        {
            if (!g_tracked[i].gone)
                LogGone(g_tracked[i]);
            g_tracked.erase(g_tracked.begin() + i);
        }
        else
            ++i;
    for (size_t i = 0; i < n; ++i)
    {
        float centre[3] = {}, r = 0.0f;
        bool up = SmokeNow(smokes[i], centre, &r);
        if (Tracked* t = TrackedFor(smokes[i]))
        {
            if (!up && !t->gone)
            {
                t->gone = true;  // shrunk away or switched off while the entity lingers
                LogGone(*t);
            }
            continue;
        }
        if (!up)
            continue;
        Tracked t;
        t.smoke = smokes[i];
        std::memcpy(t.pos, centre, sizeof(t.pos));
        t.pos[1] -= kSmokeCentreHeight;
        t.upAt = GetTickCount64();
        g_tracked.push_back(t);
        void* data = nullptr;
        float life = 0.0f, shrink = 0.0f;
        uint8_t fallOff = 0;
        if (eng::ReadPtr(eng::At(smokes[i], kEntity_Data), &data) && data)
        {
            eng::ReadFloat(eng::At(data, kSmokeData_LifeTime), &life);
            eng::ReadFloat(eng::At(data, kSmokeData_ShrinkDelay), &shrink);
            eng::ReadByte(eng::At(data, kSmokeData_RadiusTimeFallOff), &fallOff);
        }
        Log("grenades: smoke screen up at (%.1f, %.1f, %.1f): radius %.1f m, lasts %.1f s%s (%u up now)", centre[0],
            centre[1] - kSmokeCentreHeight, centre[2], r, life,
            fallOff ? (shrink < life ? ", shrinking toward the end" : "") : "", static_cast<unsigned>(n));
    }
}

}  // namespace grenades
