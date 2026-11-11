#include "guards.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "engine.h"
#include "hook.h"
#include "log.h"

namespace {

bool OnlineStatsMissing()
{
    return *reinterpret_cast<void* const volatile*>(eng::kOnlineStatsPtr) == nullptr;
}

// Kill handler 0xE75030: `mov edx, [stats]; mov esi, [edx+0x3C]` at 0xE75631 (9 bytes) becomes a jmp to this stub,
// which takes the existing "stats lookup found nothing" exit (0xE7579A) when the object is missing.
constexpr uintptr_t kKillStatsSkip = 0xE7579A;
constexpr uintptr_t kKillStatsResume = 0xE7563A;

bool PatchKillStats()
{
    auto* site = reinterpret_cast<uint8_t*>(eng::Address("KillStats_readOnlineStats"));
    if (!site)
        return false;
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub)
        return false;
    uint8_t* p = stub;
    auto rel32 = [](const uint8_t* next, uintptr_t target) {
        return static_cast<int32_t>(target - reinterpret_cast<uintptr_t>(next));
    };
    // mov edx, [stats]
    *p++ = 0x8B;
    *p++ = 0x15;
    uint32_t stats = static_cast<uint32_t>(eng::kOnlineStatsPtr);
    std::memcpy(p, &stats, 4);
    p += 4;
    // test edx, edx ; jz skip
    *p++ = 0x85;
    *p++ = 0xD2;
    *p++ = 0x0F;
    *p++ = 0x84;
    int32_t skip = rel32(p + 4, kKillStatsSkip);
    std::memcpy(p, &skip, 4);
    p += 4;
    // mov esi, [edx+0x3C] ; jmp resume
    *p++ = 0x8B;
    *p++ = 0x72;
    *p++ = 0x3C;
    *p++ = 0xE9;
    int32_t resume = rel32(p + 4, kKillStatsResume);
    std::memcpy(p, &resume, 4);
    p += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, p - stub);

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    site[0] = 0xE9;
    int32_t toStub = rel32(site + 5, reinterpret_cast<uintptr_t>(stub));
    std::memcpy(site + 1, &toStub, 4);
    std::memset(site + 5, 0x90, 4);
    VirtualProtect(site, 9, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, 9);
    return true;
}

// Generic guard for `len` bytes at a signature-checked site whose first instruction dereferences eax: jump to a stub
// that does `test eax, eax; jz nullExit`, replays the original (position-independent) bytes, and jumps back.
bool PatchEaxNullGuard(const char* sigName, size_t len, uintptr_t nullExit)
{
    auto* site = reinterpret_cast<uint8_t*>(eng::Address(sigName));
    if (!site || len < 5 || len > 16)
        return false;
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub)
        return false;
    auto rel32 = [](const uint8_t* next, uintptr_t target) {
        return static_cast<int32_t>(target - reinterpret_cast<uintptr_t>(next));
    };
    uint8_t* p = stub;
    *p++ = 0x85;  // test eax, eax
    *p++ = 0xC0;
    *p++ = 0x0F;  // jz nullExit
    *p++ = 0x84;
    int32_t toExit = rel32(p + 4, nullExit);
    std::memcpy(p, &toExit, 4);
    p += 4;
    std::memcpy(p, site, len);  // the original instruction(s)
    p += len;
    *p++ = 0xE9;  // jmp back
    int32_t back = rel32(p + 4, reinterpret_cast<uintptr_t>(site) + len);
    std::memcpy(p, &back, 4);
    p += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, p - stub);

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    site[0] = 0xE9;
    int32_t toStub = rel32(site + 5, reinterpret_cast<uintptr_t>(stub));
    std::memcpy(site + 1, &toStub, 4);
    std::memset(site + 5, 0x90, len - 5);
    VirtualProtect(site, len, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return true;
}

// Client death screen 0x17D50E0: `mov eax, [eax+0x160]` on the killer's persistence record (null when a bot killed
// you; bots have none) -> its own exit 0x17D51D6 (pop edi; pop esi; add esp, 0x10; ret 0x10), leaving the killer's
// rank text empty.
constexpr uintptr_t kDeathScreenExit = 0x17D51D6;

// Client teammate check 0x95ECC0 (your soldier overlapping an AI teammate's, e.g. spawning on a bot fireteam buddy):
// `mov eax, [LevelData.AISystem + 0xC]` with no AISystem on MP levels. On null continue at 0x95EDAB with ZF set,
// the code's own "no AI setting" path (bl = 1).
constexpr uintptr_t kTeammateCheckNoAiSetting = 0x95EDAB;

// Explosion spotting 0xE030A0 (an explosion whose VeniceExplosionEntityData spots or unspots what it hits): after
// getGiverPlayer (0xBAE090) it reads the thrower's team, `mov ecx, [eax+0x38C]`, unchecked. The giver's player is
// gone when a bot's grenade lands after the bot died (the game destroys a dead AI player) -> crash at 0xE031FD. On
// null take the function's own exit 0xE0324F (pop edi; pop esi; pop ebx; add esp, 0xC; ret 0xC): no spot change.
constexpr uintptr_t kExplosionSpotExit = 0xE0324F;

// Score/awards routine 0x169BB40: the score is awarded first (0x169AD40); only the award loop after it asks the online
// stats object. The site `mov ecx, [0x2B14928]` (6 bytes, absolute - replayable as is) jumps to a stub that replays
// it, then `test ecx, ecx; jz` the loop's own exit 0x169BC8D (pop esi; pop ebp; ...), keeping the score award.
constexpr uintptr_t kAwardsLoopExit = 0x169BC8D;

bool PatchAwardsLoop()
{
    auto* site = reinterpret_cast<uint8_t*>(eng::Address("Awards_readOnlineStats"));
    if (!site)
        return false;
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub)
        return false;
    auto rel32 = [](const uint8_t* next, uintptr_t target) {
        return static_cast<int32_t>(target - reinterpret_cast<uintptr_t>(next));
    };
    uint8_t* p = stub;
    std::memcpy(p, site, 6);  // mov ecx, [0x2B14928]
    p += 6;
    *p++ = 0x85;  // test ecx, ecx
    *p++ = 0xC9;
    *p++ = 0x0F;  // jz loop exit
    *p++ = 0x84;
    int32_t toExit = rel32(p + 4, kAwardsLoopExit);
    std::memcpy(p, &toExit, 4);
    p += 4;
    *p++ = 0xE9;  // jmp back
    int32_t back = rel32(p + 4, reinterpret_cast<uintptr_t>(site) + 6);
    std::memcpy(p, &back, 4);
    p += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, p - stub);

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    site[0] = 0xE9;
    int32_t toStub = rel32(site + 5, reinterpret_cast<uintptr_t>(stub));
    std::memcpy(site + 1, &toStub, 4);
    site[5] = 0x90;
    VirtualProtect(site, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    return true;
}

// Deploy-stats onMessage 0x169C680 (thiscall, 1 arg, void).
using OnMessageFn = void(__thiscall*)(void* self, void* message);
OnMessageFn g_deployStats = nullptr;

void __fastcall DeployStatsDetour(void* self, void* /*edx*/, void* message)
{
    if (OnlineStatsMissing())
        return;
    g_deployStats(self, message);
}

// Client voice chat. The voice manager (0x2B44478) passes VoipGetRef() ([0x2B4447C]) to VoipControl for its speaker
// and mic channel masks (0x1C20100 'uspk', 0x1C200E0 'umic', from 0x1C20380 / 0x1C20E30 / 0x1C21600), and VoipControl
// reads it unchecked (0x1C29312: [ref+0x48]). Offline no voice session starts, so the ref is null: the voice settings
// message (0x904AA3D9, value 7 = VOIP off, sent from the UI as it reads the `VOIPOn` profile option - pressing Tab
// while dead) crashed the game. With no ref there's nothing to set: return as VoipControl does for an error.
using VoipMaskFn = int(__cdecl*)(void* ref, int mask);
VoipMaskFn g_speakerMask = nullptr;
VoipMaskFn g_micMask = nullptr;
volatile LONG g_voipSkipsLogged = 0;

void NoteVoipSkip(const char* what)
{
    if (InterlockedCompareExchange(&g_voipSkipsLogged, 1, 0) == 0)
        Log("voice: %s channel change skipped - no voice session (offline); the game used to crash here", what);
}

int __cdecl SpeakerMaskDetour(void* ref, int mask)
{
    if (!ref)
    {
        NoteVoipSkip("speaker");
        return -1;
    }
    return g_speakerMask(ref, mask);
}

int __cdecl MicMaskDetour(void* ref, int mask)
{
    if (!ref)
    {
        NoteVoipSkip("mic");
        return -1;
    }
    return g_micMask(ref, mask);
}

}  // namespace

bool InstallOfflineStatsGuards()
{
    bool kill = PatchKillStats();
    bool awards = PatchAwardsLoop();
    void* deploy = reinterpret_cast<void*>(eng::Address("DeploymentStats_onMessage"));
    g_deployStats = reinterpret_cast<OnMessageFn>(
        deploy ? InlineHook(deploy, 6, reinterpret_cast<const void*>(&DeployStatsDetour)) : nullptr);
    bool deathScreen = PatchEaxNullGuard("DeathScreen_readKillerRank", 6, kDeathScreenExit);
    bool teammate = PatchEaxNullGuard("TeammateCheck_readAISystem", 5, kTeammateCheckNoAiSetting);
    bool explosion = PatchEaxNullGuard("ExplosionSpot_readGiverTeam", 6, kExplosionSpotExit);
    if (void* speaker = reinterpret_cast<void*>(eng::Address("Voip_setSpeakerMask")))
        g_speakerMask = reinterpret_cast<VoipMaskFn>(InlineHook(speaker, 8, reinterpret_cast<const void*>(&SpeakerMaskDetour)));
    if (void* mic = reinterpret_cast<void*>(eng::Address("Voip_setMicMask")))
        g_micMask = reinterpret_cast<VoipMaskFn>(InlineHook(mic, 8, reinterpret_cast<const void*>(&MicMaskDetour)));
    bool voice = g_speakerMask && g_micMask;
    Log("offline guards: kill handler %s, awards loop %s, deploy stats %s, death screen %s, AI-teammate check %s, "
        "explosion spotting %s, voice channels %s (online stats object %s)",
        kill ? "patched" : "FAILED", awards ? "patched" : "FAILED", g_deployStats ? "hooked" : "FAILED",
        deathScreen ? "patched" : "FAILED", teammate ? "patched" : "FAILED", explosion ? "patched" : "FAILED",
        voice ? "guarded" : "FAILED", OnlineStatsMissing() ? "missing" : "present");
    return kill && awards && g_deployStats && deathScreen && teammate && explosion && voice;
}
