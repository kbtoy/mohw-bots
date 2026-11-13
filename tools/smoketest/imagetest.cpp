// Offline test against the real game code: copies lanmohw.exe's in-memory image (dumped to a file) to its real
// addresses from 0x500000 up, then loads the bot DLL. Checks: signature verification passes, the Server::update
// hook is written correctly, and the engine's own EntryInput ctor/setters produce the layout in RE_NOTES.md.
// Build fixed at 0x400000 (see CMakeLists.txt). Usage: imagetest.exe <lanmohw.image.bin> <xinput1_3.dll>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uintptr_t kImageBase = 0x400000;
constexpr uintptr_t kLowestNeeded = 0x418000;  // lowest game address the DLL touches (startup-args patch)
constexpr size_t kGameImageSize = 0x2865000;

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

using Ctor = void*(__thiscall*)(void*);
using SetInt = void(__thiscall*)(void*, int);
using SetBoolAction = void(__thiscall*)(void*, int, int);
using SetFloat = void(__thiscall*)(void*, float);
using GetLevel = float(__thiscall*)(void*, int);

alignas(16) uint8_t g_input[0x600];

// Runs a KeepPlayers delete-site stub as the game would (player in edi): returns ZF after it (1 = keep the player,
// the game's skip) and whether ecx / edx came back unchanged.
int RunDeleteStub(const void* stub, void* player, bool* registersKept)
{
    int zf = 0, ecxOut = 0, edxOut = 0;
    __asm {
        push edi
        mov edi, player
        mov ecx, 0x11111111
        mov edx, 0x22222222
        call stub
        setz al
        movzx eax, al
        mov zf, eax
        mov ecxOut, ecx
        mov edxOut, edx
        pop edi
    }
    *registersKept = ecxOut == 0x11111111 && edxOut == 0x22222222;
    return zf;
}

// Calls a voice channel mask wrapper (cdecl(voipRef, mask)) with a null ref, as offline; 0x7FFFFFFF if it faulted.
int CallVoipMask(uintptr_t wrapper)
{
    using MaskFn = int(__cdecl*)(void* ref, int mask);
    __try
    {
        return reinterpret_cast<MaskFn>(wrapper)(nullptr, 0x3F);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0x7FFFFFFF;
    }
}

// Each step is a separate guarded call so a fault names the step instead of killing the test.
template <class Fn>
bool Step(const char* name, Fn fn)
{
    __try
    {
        fn();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        std::printf("  fault 0x%08lX in: %s\n", GetExceptionCode(), name);
        return false;
    }
}

void TestEntryInput()
{
    std::memset(g_input, 0xCD, sizeof(g_input));
    if (Step("EntryInput ctor 0x80EF00", [] { reinterpret_cast<Ctor>(0x80EF00)(g_input); }))
        Check(*reinterpret_cast<uint32_t*>(g_input + 0x3A8) == 0 && *reinterpret_cast<uint32_t*>(g_input + 0x528) == 0,
              "EntryInput ctor clears fields up to +0x528");
    else
        Check(false, "EntryInput ctor runs");

    if (Step("setAuthoritativeAiming 0x800780", [] { reinterpret_cast<SetInt>(0x800780)(g_input, 1); }))
        Check((*reinterpret_cast<uint32_t*>(g_input + 0x3C0) & 1) == 1, "setAuthoritativeAiming sets flags bit0 (+0x3C0)");

    if (Step("aiming yaw/pitch setters", [] {
            reinterpret_cast<SetFloat>(0x8007C0)(g_input, 1.25f);
            reinterpret_cast<SetFloat>(0x800810)(g_input, -0.5f);
        }))
        Check(*reinterpret_cast<float*>(g_input + 0x3CC) == 1.25f && *reinterpret_cast<float*>(g_input + 0x3D0) == -0.5f,
              "aiming yaw at +0x3CC, pitch at +0x3D0");

    *reinterpret_cast<float*>(g_input) = 1.0f;  // EIAThrottle written the way the DLL does it
    float throttle = 0.0f;
    if (Step("getLevel 0x800460", [&] { throttle = reinterpret_cast<GetLevel>(0x800460)(g_input, 0); }))
        Check(throttle == 1.0f, "getLevel(EIAThrottle) reads analog[0] at +0x000");

    float jump = 0.0f;
    if (Step("setLevelBool + getLevel", [&] {
            reinterpret_cast<SetBoolAction>(0x800540)(g_input, 14, 1);  // EIAJump
            jump = reinterpret_cast<GetLevel>(0x800460)(g_input, 14);
        }))
        Check(((*reinterpret_cast<uint64_t*>(g_input + 0x3B8) >> 2) & 1) == 1 && jump == 1.0f,
              "digital EIAJump (14) is bit 2 of +0x3B8");
}

// In-place level change (dll/src/engine.cpp RequestLevelLoad): the game's own constructors lay out what the DLL fills in.
alignas(16) uint8_t g_request[0x60];
alignas(16) uint8_t g_load[0x50];
alignas(16) uint8_t g_reset[0x40];

void TestLevelChangeMessages()
{
    std::memset(g_request, 0xCD, sizeof(g_request));
    if (Step("NetworkRequestLoadLevelMessage ctor 0x258C490", [] { reinterpret_cast<Ctor>(0x258C490)(g_request); }))
    {
        auto u32 = [](const uint8_t* p, int at) { return *reinterpret_cast<const uint32_t*>(p + at); };
        Check(u32(g_request, 0) == 0x278B9FC && u32(g_request, 4) == 0xC67D277B && u32(g_request, 8) == 0x3AFF49D1 &&
                  u32(g_request, 0x38) == 0x2A53AE8 && u32(g_request, 0x3C) == 0x2A7DA24 && u32(g_request, 0x40) == 0 &&
                  u32(g_request, 0x44) == 0x2A7DA24 && u32(g_request, 0x48) == 0x2A7DA24 && u32(g_request, 0x4C) == 0xFFFFFFFF &&
                  g_request[0x50] == 0 && g_request[0x51] == 0,
              "level change: the request's LevelSetup at +0x38 (empty name, no options, difficulty 0, ForceReload at +0x18 clear)");
    }
    else
        Check(false, "level change: NetworkRequestLoadLevelMessage ctor runs");
    std::memset(g_load, 0xCD, sizeof(g_load));
    if (Step("ServerLoadLevelMessage base ctor 0x259EB00", [] { reinterpret_cast<Ctor>(0x259EB00)(g_load); }))
        Check(*reinterpret_cast<uint32_t*>(g_load + 4) == 0xC6E40BA0 && *reinterpret_cast<uint32_t*>(g_load + 8) == 0xF8429A0E &&
                  *reinterpret_cast<const uint32_t*>(0x27AD710) == 0x259EB20,
              "level change: ServerLoadLevelMessage is category 0xC6E40BA0 type 0xF8429A0E, its vtable's getType 0x259EB20");
    std::memset(g_reset, 0xCD, sizeof(g_reset));
    if (Step("ServerRoundResetMessage ctor 0x25A0780", [] { reinterpret_cast<Ctor>(0x25A0780)(g_reset); }))
        Check(*reinterpret_cast<uint32_t*>(g_reset + 4) == 0xD93DD7C2 && *reinterpret_cast<uint32_t*>(g_reset + 8) == 0x4BB8DE5C,
              "level change: ServerRoundResetMessage is category 0xD93DD7C2 type 0x4BB8DE5C (what bots.cpp listens for)");
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
        return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Zero-filled .bss that, with the exe fixed at 0x400000, covers 0x500000..end of the game image; the loader
    // reserves it with the exe, so nothing else can occupy that range.
    static uint8_t space[kGameImageSize];
    auto spaceBegin = reinterpret_cast<uintptr_t>(space);

    FILE* f = _wfopen(argv[1], L"rb");
    if (!f)
        return 3;
    std::vector<uint8_t> image;
    fseek(f, 0, SEEK_END);
    image.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    fread(image.data(), 1, image.size(), f);
    fclose(f);

    // Copy the game image into this exe's .bss from the first page past our own code/data onwards.
    const uintptr_t kMapFrom = (spaceBegin + 0xFFF) & ~uintptr_t(0xFFF);
    size_t length = image.size() - (kMapFrom - kImageBase);
    bool covers = kMapFrom <= kLowestNeeded && spaceBegin + sizeof(space) >= kMapFrom + length;
    std::printf("  game image copied from 0x%08X\n", static_cast<unsigned>(kMapFrom));
    Check(covers, "reserved .bss covers every game address the DLL touches");
    if (!covers)
        return 4;
    void* mapped = reinterpret_cast<void*>(kMapFrom);
    DWORD oldProtect = 0;
    VirtualProtect(mapped, length, PAGE_EXECUTE_READWRITE, &oldProtect);
    std::memcpy(mapped, image.data() + (kMapFrom - kImageBase), length);
    // A raw image copy has no resolved imports; EntryInput's reset calls memset/memcpy through these IAT slots.
    *reinterpret_cast<void**>(0x2B4CC94) = reinterpret_cast<void*>(&std::memset);
    *reinterpret_cast<void**>(0x2B4CC90) = reinterpret_cast<void*>(&std::memcpy);

    HMODULE dll = LoadLibraryW(argv[2]);
    Check(dll != nullptr, "load bot DLL");

    auto* target = reinterpret_cast<uint8_t*>(0xAA8DE0);
    Check(target[0] == 0xE9 && target[5] == 0x90 && target[9] == 0x90, "Server::update patched with jmp + nops");
    auto* detour = target + 5 + *reinterpret_cast<int32_t*>(target + 1);
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(detour, &mbi, sizeof(mbi));
    Check(mbi.AllocationBase == dll, "jmp lands inside the bot DLL");

    // Startup-args patch (needs build/Release/mohw-bots.ini with [launch] StartupArgs set; see the DLL README).
    const char* pushed = *reinterpret_cast<const char**>(0x418183);
    VirtualQuery(pushed, &mbi, sizeof(mbi));
    bool inDll = mbi.AllocationBase == dll;
    Check(inDll && std::strncmp(pushed, "-webmode spoffline -level ", 26) == 0,
          "offline-args push now points at the DLL's \"-webmode spoffline -level ...\" string");
    if (inDll)
        std::printf("  startup args: %s\n", pushed);

    const auto* originRead = reinterpret_cast<const uint8_t*>(0xDC4211);
    Check(originRead[0] == 0x32 && originRead[1] == 0xC0 && originRead[2] == 0x90 && originRead[3] == 0x88,
          "DisableOrigin: EnableOrigin read replaced with xor al, al; nop");

    Check(*reinterpret_cast<const uint8_t*>(0xD257BF) == 0xEB, "ForceLocalServerBackend: backend choice always jumps to Backend_Local");
    {
        const auto* limit1 = reinterpret_cast<const uint8_t*>(0x61D6EA);
        const auto* limit2 = reinterpret_cast<const uint8_t*>(0x61D703);
        std::printf("  challenge retry limits: %u, %u (game default 15)\n", limit1[6], limit2[6]);
        Check(limit1[0] == 0x83 && limit1[1] == 0xBE && limit2[0] == 0x83 && limit2[1] == 0xBE && limit1[6] == limit2[6] &&
                  limit1[6] != 0x0F,
              "ChallengeRetries: both challenge-state retry limits raised, instructions intact");

        // Offline stats guards: the kill handler's unchecked read jumps to a stub that tests the pointer first.
        const auto* killStats = reinterpret_cast<const uint8_t*>(0xE75631);
        const uint8_t* stub = killStats + 5 + *reinterpret_cast<const int32_t*>(killStats + 1);
        bool stubOk = killStats[0] == 0xE9 && killStats[5] == 0x90 && killStats[8] == 0x90 && stub[0] == 0x8B &&
                      stub[1] == 0x15 && *reinterpret_cast<const uint32_t*>(stub + 2) == 0x2B14928 && stub[6] == 0x85 &&
                      stub[8] == 0x0F && stub[9] == 0x84 &&
                      stub + 14 + *reinterpret_cast<const int32_t*>(stub + 10) == reinterpret_cast<const uint8_t*>(0xE7579A) &&
                      stub[14] == 0x8B && stub[15] == 0x72 && stub[16] == 0x3C && stub[17] == 0xE9 &&
                      stub + 22 + *reinterpret_cast<const int32_t*>(stub + 18) == reinterpret_cast<const uint8_t*>(0xE7563A);
        Check(stubOk, "OfflineStatsGuards: kill handler null-checks the stats object (skip 0xE7579A, resume 0xE7563A)");

        const auto* death = reinterpret_cast<const uint8_t*>(0x17D5123);
        const uint8_t* deathStub = death + 5 + *reinterpret_cast<const int32_t*>(death + 1);
        bool deathOk = death[0] == 0xE9 && death[5] == 0x90 && deathStub[0] == 0x85 && deathStub[1] == 0xC0 &&
                       deathStub[2] == 0x0F && deathStub[3] == 0x84 &&
                       deathStub + 8 + *reinterpret_cast<const int32_t*>(deathStub + 4) ==
                           reinterpret_cast<const uint8_t*>(0x17D51D6) &&
                       std::memcmp(deathStub + 8, "\x8B\x80\x60\x01\x00\x00", 6) == 0 && deathStub[14] == 0xE9 &&
                       deathStub + 19 + *reinterpret_cast<const int32_t*>(deathStub + 15) ==
                           reinterpret_cast<const uint8_t*>(0x17D5129);
        Check(deathOk, "Death screen: killer-rank read null-checked (exit 0x17D51D6, resume 0x17D5129)");

        const auto* mate = reinterpret_cast<const uint8_t*>(0x95EDA6);
        const uint8_t* mateStub = mate + 5 + *reinterpret_cast<const int32_t*>(mate + 1);
        bool mateOk = mate[0] == 0xE9 && mateStub[0] == 0x85 && mateStub[1] == 0xC0 && mateStub[2] == 0x0F &&
                      mateStub[3] == 0x84 &&
                      mateStub + 8 + *reinterpret_cast<const int32_t*>(mateStub + 4) ==
                          reinterpret_cast<const uint8_t*>(0x95EDAB) &&
                      std::memcmp(mateStub + 8, "\x8B\x40\x0C\x85\xC0", 5) == 0 && mateStub[13] == 0xE9 &&
                      mateStub + 18 + *reinterpret_cast<const int32_t*>(mateStub + 14) ==
                          reinterpret_cast<const uint8_t*>(0x95EDAB);
        Check(mateOk, "AI-teammate check: missing LevelData.AISystem null-checked (continues at 0x95EDAB)");
        auto* boom = reinterpret_cast<const uint8_t*>(0xE031FD);
        auto* boomStub = boom + 5 + *reinterpret_cast<const int32_t*>(boom + 1);
        bool boomOk = boom[0] == 0xE9 && boom[5] == 0x90 && boomStub[0] == 0x85 && boomStub[1] == 0xC0 &&
                      boomStub[2] == 0x0F && boomStub[3] == 0x84 &&
                      boomStub + 8 + *reinterpret_cast<const int32_t*>(boomStub + 4) ==
                          reinterpret_cast<const uint8_t*>(0xE0324F) &&
                      boomStub[8] == 0x8B && boomStub[9] == 0x88;
        Check(boomOk, "Explosion spotting: missing thrower player null-checked (exit 0xE0324F)");
        // Minimap: the per-player pass's "enemy AI soldier -> skip" je became jmp (same target), the rest intact.
        const auto* skipAI = reinterpret_cast<const uint8_t*>(0x178635B);
        Check(skipAI[0] == 0xEB && skipAI[1] == 0x0E && std::memcmp(skipAI + 2, "\x8B\x44\x24\x3C\x39\x44\x24\x14", 8) == 0 &&
                  *reinterpret_cast<const uint8_t*>(0x1786359) == 0x84,
              "Minimap: enemy AI soldiers no longer skipped (0x178635B je -> jmp +0x0E)");
        // Server animation: both game-type skips (singleplayer, coop) are NOPs; the ServerEnable check after them stays.
        const auto* animSp = reinterpret_cast<const uint8_t*>(0xAA9FCE);
        const auto* animCoop = reinterpret_cast<const uint8_t*>(0xAA9FDD);
        Check(std::memcmp(animSp, "\x84\xC0\x90\x90\x90\x90\x90\x90", 8) == 0 &&
                  std::memcmp(animCoop, "\x84\xC0\x90\x90\x90\x90\x90\x90", 8) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xAA9FF4), "\x8B\x0D\xF0\x5D\xAC\x02\x38\x59\x44", 9) == 0,
              "ForceServerAnimation: server animation manager created in any game type (0xAA9FD0 / 0xAA9FDF jne -> nops)");
        // Bot bullets as players: the damage sum's "AI shooter -> x weapon AI multiplier" step is jumped over (je ->
        // jmp, same target 0xD3CF50); what it skips is the call through the soldier's +0x130 interface slot +0x74
        // (0xB5ECB0: current weapon's AIData +0x10 friendly / +0x0C otherwise).
        const auto* aiMul = reinterpret_cast<const uint8_t*>(0xD3CF1B);
        Check(std::memcmp(aiMul, "\x80\xBE\x54\x03\x00\x00\x00\xEB\x2C", 9) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xD3CF50), "\x85\xF6", 2) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xD3CF3A), "\x8B\x52\x74", 3) == 0 &&
                  *reinterpret_cast<const uint32_t*>(0x27BA700 + 0x74) == 0xB5ECB0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xB5ECD9), "\xD9\x40\x10\xC2\x04\x00\xD9\x40\x0C", 9) == 0,
              "BotBulletsAsPlayers: AI shooters' weapon damage multiplier skipped (0xD3CF22 je -> jmp)");
        // Bot health from the server: 0x962DB0 says no for an AI or no giver on an AI victim (its jne now reaches
        // xor al, al; ret 8), the health states never pick the soldier's own estimate, and a bot's ghost update keeps
        // the server's last-hit data. limp.cpp reads the soldier's module (+0x54C, as 0x964380 calls it) and its copy.
        Check(std::memcmp(reinterpret_cast<const uint8_t*>(0x962E18), "\x80\xB9\x54\x03\x00\x00\x00\x75\xB3", 9) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0x962DD4), "\x32\xC0\xC2\x08\x00", 5) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xDCFC20), "\x33\xC0\xC3", 3) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xDCF4C2), "\x80\xBE\x54\x03\x00\x00\x00\x90\x90", 9) == 0,
              "BotHealthFromServer: your game follows the server's health for bots (0x962E20, 0xDCFC20, 0xDCF4C9)");
        Check(*reinterpret_cast<const uint32_t*>(0x27A09B8 + 0x104) == 0x964380 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0x9643BD), "\x8B\x8B\x4C\x05\x00\x00", 6) == 0 &&
                  *reinterpret_cast<const uint32_t*>(0x2814A18 + 0x7C) == 0xDD55F0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xDD55F0), "\xD9\x81\xAC\x05\x00\x00\xC3", 7) == 0,
              "BotHealthFromServer: soldier +0x54C is its health module, module +0x5AC the server's health");
        // Stealth melee: an AI victim is accepted as a human is (je -> jmp, same target, which stores the victim);
        // the component is at soldier +0x420 and the server's start jumps into the DLL.
        {
            const auto* start = reinterpret_cast<const uint8_t*>(0xB844D0);
            const uint8_t* startDetour = start + 5 + *reinterpret_cast<const int32_t*>(start + 1);
            VirtualQuery(startDetour, &mbi, sizeof(mbi));
            Check(std::memcmp(reinterpret_cast<const uint8_t*>(0x98197C), "\x8D\x8B\xB0\x00\x00\x00\xFF\xD0\x84\xC0\xEB\x21", 12) == 0 &&
                      std::memcmp(reinterpret_cast<const uint8_t*>(0x9819A9), "\x89\x5C\x24\x18", 4) == 0 &&
                      std::memcmp(reinterpret_cast<const uint8_t*>(0x958D60), "\x8B\x8F\x20\x04\x00\x00", 6) == 0 &&
                      start[0] == 0xE9 && start[5] == 0x90 && mbi.AllocationBase == dll,
                  "StealthMeleeOnBots: an AI victim is accepted (0x981986 je -> jmp); server start hooked");
        // ProCon's admin.say: the function that shows a chat message (ret 4, no ecx) and the ADMIN channel test.
        Check(std::memcmp(reinterpret_cast<const uint8_t*>(0x873290), "\xA1\xE4\x5F\xAC\x02\x8B\x48\x34", 8) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0x8732B3), "\x83\xBF\x18\x01\x00\x00\x04", 7) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xF55397), "\x83\xBF\x18\x01\x00\x00\x04", 7) == 0,
              "rcon: admin.say goes through 0x873290, channel 4 needs no player and shows as ADMIN");
        // Hardcore: an AI soldier skips the team factor (first je now lands on the second test) but gets the health setting.
        Check(std::memcmp(reinterpret_cast<const uint8_t*>(0xB67E6B), "\x84\xDB\x74\x1F", 4) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xB67E8E), "\x84\xDB\x90\x90\xA1\x98\xD4\xA4\x02", 9) == 0 &&
                  std::memcmp(reinterpret_cast<const uint8_t*>(0xB67EB6), "\xF3\x0F\x10\x40\x2C", 5) == 0,
              "hardcore: the soldier health setting applies to bots too (0xB67E00)");
        }
        {
            const auto* entry = reinterpret_cast<const uint8_t*>(0x169C680);
            const uint8_t* detour = entry + 5 + *reinterpret_cast<const int32_t*>(entry + 1);
            VirtualQuery(detour, &mbi, sizeof(mbi));
            Check(entry[0] == 0xE9 && mbi.AllocationBase == dll, "OfflineStatsGuards: deploy stats handler jumps into the DLL");
        }
        {
            // Voice channel masks: both wrappers jump into the DLL, and with no voice session (a null VoipRef, as
            // offline) they return -1 instead of reading [ref+0x48] in VoipControl.
            bool jumps = true;
            int results[2] = {0, 0};
            const uintptr_t wrappers[2] = {0x1C20100, 0x1C200E0};
            for (int i = 0; i < 2; ++i)
            {
                const auto* entry = reinterpret_cast<const uint8_t*>(wrappers[i]);
                const uint8_t* detour = entry + 5 + *reinterpret_cast<const int32_t*>(entry + 1);
                VirtualQuery(detour, &mbi, sizeof(mbi));
                jumps = jumps && entry[0] == 0xE9 && mbi.AllocationBase == dll;
                results[i] = CallVoipMask(wrappers[i]);
            }
            Check(jumps && results[0] == -1 && results[1] == -1,
                  "voice: speaker / mic mask wrappers (0x1C20100, 0x1C200E0) return -1 with no voice session instead of crashing");
        }
        {
            const auto* entry = reinterpret_cast<const uint8_t*>(0xB64FE0);
            const uint8_t* detour = entry + 5 + *reinterpret_cast<const int32_t*>(entry + 1);
            VirtualQuery(detour, &mbi, sizeof(mbi));
            Check(entry[0] == 0xE9 && mbi.AllocationBase == dll && entry[5] == 0xF6 && entry[6] == 0x43,
                  "combat log: soldier damage entry (0xB64FE0) jumps into the DLL, the rest intact");
            const auto* claim = reinterpret_cast<const uint8_t*>(0xE41590);
            const uint8_t* claimDetour = claim + 5 + *reinterpret_cast<const int32_t*>(claim + 1);
            VirtualQuery(claimDetour, &mbi, sizeof(mbi));
            Check(claim[0] == 0xE9 && mbi.AllocationBase == dll && claim[6] == 0x81 && claim[7] == 0xEC,
                  "BotHitsOnBotsByServer: hit-claim handler (0xE41590) jumps into the DLL, the rest intact");
        }
        {
            const auto* filter = reinterpret_cast<const uint8_t*>(0x26BC090);
            const uint8_t* filterDetour = filter + 5 + *reinterpret_cast<const int32_t*>(filter + 1);
            VirtualQuery(filterDetour, &mbi, sizeof(mbi));
            bool filterOk = filter[0] == 0xE9 && mbi.AllocationBase == dll && filter[9] == 0x50 && filter[10] == 0x68;
            const auto* team = reinterpret_cast<const uint8_t*>(0x25D0770);
            const uint8_t* teamDetour = team + 5 + *reinterpret_cast<const int32_t*>(team + 1);
            VirtualQuery(teamDetour, &mbi, sizeof(mbi));
            Check(filterOk && team[0] == 0xE9 && mbi.AllocationBase == dll && team[6] == 0x50 && team[7] == 0x68,
                  "FallBackToOwnBase: spawn chooser TeamFilter (0x26BC090) and spawn point Team (0x25D0770) getters "
                  "jump into the DLL, the rest intact");
        }
        {
            // KeepPlayers: `cmp byte [player+0x354], 0; je skip` -> `call stub; je skip; nop; nop`, the je still landing
            // on the original target; the stub pushes the player's register and calls into the DLL.
            struct Site { uintptr_t at; uint8_t push; uint8_t skip; uint8_t next0; uint8_t next1; };
            const Site sites[] = {{0xB61C08, 0x57, 0x16, 0x8B, 0xCF}, {0xB6BE4B, 0x57, 0x15, 0x8B, 0xCF},
                                  {0xB18B0F, 0x56, 0x15, 0x8B, 0xCE}};
            bool ok = true;
            for (const Site& s : sites)
            {
                const auto* site = reinterpret_cast<const uint8_t*>(s.at);
                const uint8_t* stub = site + 5 + *reinterpret_cast<const int32_t*>(site + 1);
                const uint8_t* fn = stub + 8 + *reinterpret_cast<const int32_t*>(stub + 4);
                VirtualQuery(fn, &mbi, sizeof(mbi));
                ok &= site[0] == 0xE8 && site[5] == 0x74 && site + 7 + site[6] == reinterpret_cast<const uint8_t*>(s.at + 9 + s.skip) &&
                      site[7] == 0x90 && site[8] == 0x90 && site[9] == s.next0 && site[10] == s.next1 &&
                      stub[0] == 0x51 && stub[1] == 0x52 && stub[2] == s.push && stub[3] == 0xE8 &&
                      std::memcmp(stub + 8, "\x83\xC4\x04\x5A\x59\x84\xC0\xC3", 8) == 0 && mbi.AllocationBase == dll;
            }
            Check(ok, "KeepPlayers: the three AI-player delete sites ask the DLL first (call stub; je skip), skip targets kept");
            {
                // A human (isAI 0) is skipped as before; an AI player that isn't a bot of ours is still deleted.
                const auto* site = reinterpret_cast<const uint8_t*>(0xB61C08);
                const uint8_t* stub = site + 5 + *reinterpret_cast<const int32_t*>(site + 1);
                alignas(16) static uint8_t fakePlayer[0x400] = {};
                bool humanRegs = false, aiRegs = false;
                fakePlayer[0x354] = 0;
                int humanKept = RunDeleteStub(stub, fakePlayer, &humanRegs);
                fakePlayer[0x354] = 1;
                int aiKept = RunDeleteStub(stub, fakePlayer, &aiRegs);
                Check(humanKept == 1 && aiKept == 0 && humanRegs && aiRegs,
                      "KeepPlayers stub: human player skipped (ZF 1), other AI player deleted (ZF 0), ecx / edx kept");
            }
            const auto* listener = reinterpret_cast<const uint8_t*>(0xE80F40);
            const uint8_t* listenerDetour = listener + 5 + *reinterpret_cast<const int32_t*>(listener + 1);
            VirtualQuery(listenerDetour, &mbi, sizeof(mbi));
            Check(listener[0] == 0xE9 && mbi.AllocationBase == dll && listener[5] == 0x90 && listener[6] == 0x90 &&
                      listener[7] == 0x8B && listener[8] == 0x48,
                  "Scoreboard: scoring system ServerPlayer listener (0xE80F40) jumps into the DLL, the rest intact");
            // Objectives: ServerCapturePointEntity's constructor (7 bytes taken; xor ebx, ebx next) and destructor (6 bytes
            // taken; lea edi, [esi+...] next) both jump into the DLL.
            const auto* cpCtor = reinterpret_cast<const uint8_t*>(0xE450D0);
            const auto* cpDtor = reinterpret_cast<const uint8_t*>(0xE442E0);
            MEMORY_BASIC_INFORMATION ctorInfo = {}, dtorInfo = {};
            VirtualQuery(cpCtor + 5 + *reinterpret_cast<const int32_t*>(cpCtor + 1), &ctorInfo, sizeof(ctorInfo));
            VirtualQuery(cpDtor + 5 + *reinterpret_cast<const int32_t*>(cpDtor + 1), &dtorInfo, sizeof(dtorInfo));
            Check(cpCtor[0] == 0xE9 && ctorInfo.AllocationBase == dll && cpCtor[5] == 0x90 && cpCtor[6] == 0x90 && cpCtor[7] == 0x33 &&
                      cpCtor[8] == 0xDB && cpDtor[0] == 0xE9 && dtorInfo.AllocationBase == dll && cpDtor[5] == 0x90 &&
                      cpDtor[6] == 0x8D && cpDtor[7] == 0xBE,
                  "Objectives: capture point constructor (0xE450D0) and destructor (0xE442E0) jump into the DLL, the rest intact");
            // The pre-round listener: 11 bytes taken (mov eax, [esp+4]; cmp dword ptr [eax+4], imm32), push esi next.
            const auto* preRound = reinterpret_cast<const uint8_t*>(0xEDEF20);
            MEMORY_BASIC_INFORMATION preRoundInfo = {};
            VirtualQuery(preRound + 5 + *reinterpret_cast<const int32_t*>(preRound + 1), &preRoundInfo, sizeof(preRoundInfo));
            bool padded = true;
            for (int i = 5; i < 11; ++i)
                padded = padded && preRound[i] == 0x90;
            Check(preRound[0] == 0xE9 && preRoundInfo.AllocationBase == dll && padded && preRound[11] == 0x56,
                  "Objectives: pre-round listener (0xEDEF20) jumps into the DLL, the rest intact");
            // Its constructor: 5 bytes taken (mov eax, [esp+8]; push esi), push edi next.
            const auto* preCtor = reinterpret_cast<const uint8_t*>(0xEDED10);
            MEMORY_BASIC_INFORMATION preCtorInfo = {};
            VirtualQuery(preCtor + 5 + *reinterpret_cast<const int32_t*>(preCtor + 1), &preCtorInfo, sizeof(preCtorInfo));
            Check(preCtor[0] == 0xE9 && preCtorInfo.AllocationBase == dll && preCtor[5] == 0x57 && preCtor[6] == 0x33,
                  "Objectives: pre-round constructor (0xEDED10) jumps into the DLL, the rest intact");
        }
        // Awards: the routine itself runs (score is awarded); only its stats loop is guarded.
        const auto* awards = reinterpret_cast<const uint8_t*>(0x169BBCE);
        const uint8_t* awardsStub = awards + 5 + *reinterpret_cast<const int32_t*>(awards + 1);
        bool awardsOk = *reinterpret_cast<const uint8_t*>(0x169BB40) == 0x83 && awards[0] == 0xE9 && awards[5] == 0x90 &&
                        std::memcmp(awardsStub, "\x8B\x0D\x28\x49\xB1\x02\x85\xC9\x0F\x84", 10) == 0 &&
                        awardsStub + 14 + *reinterpret_cast<const int32_t*>(awardsStub + 10) ==
                            reinterpret_cast<const uint8_t*>(0x169BC8D) &&
                        awardsStub[14] == 0xE9 &&
                        awardsStub + 19 + *reinterpret_cast<const int32_t*>(awardsStub + 15) ==
                            reinterpret_cast<const uint8_t*>(0x169BBD4);
        Check(awardsOk, "OfflineStatsGuards: awards routine intact, its stats loop exits (0x169BC8D) without the object");

        // Stuck bots change class: the server's NetworkSuicideMessage branch (0xB425EE) is
        // `mov eax, [eax+0x430] ... push 1; push 0; call [vtable+0x17C]`, and ServerSoldierEntity's slot is kill.
        const auto* suicide = reinterpret_cast<const uint8_t*>(0xB425EE);
        Check(std::memcmp(suicide, "\x8B\x80\x30\x04\x00\x00", 6) == 0 && suicide[16] == 0x6A && suicide[17] == 0x01 &&
                  std::memcmp(suicide + 20, "\x8B\x82\x7C\x01\x00\x00", 6) == 0 && suicide[26] == 0x6A &&
                  suicide[27] == 0x00 && *reinterpret_cast<const uint32_t*>(0x27BA820 + 0x17C) == 0xB5E790,
              "Change class: the suicide handler calls soldier vtable +0x17C (1, 0); ServerSoldierEntity's is 0xB5E790");

        // Smoke for bots (grenades.cpp): the radius sum 0xB2A070 reads data +0x14 -> +0x81 / +0x7C / +0x74 / +0x70 and
        // age +0x94; the ctor clears active +0x98 and pushes onto the list at 0x2AF1734; 0xEAA160 walks it from 0x2AF1730.
        Check(std::memcmp(reinterpret_cast<const void*>(0xB2A071),
                          "\x8B\x41\x14\x80\xB8\x81\x00\x00\x00\x00\xF3\x0F\x10\x58\x7C\xF3\x0F\x10\x48\x74\xF3\x0F\x10\x40\x70",
                          25) == 0 &&
                  std::memcmp(reinterpret_cast<const void*>(0xB2A0AE), "\xF3\x0F\x10\x89\x94\x00\x00\x00", 8) == 0 &&
                  std::memcmp(reinterpret_cast<const void*>(0xB3E012), "\xC6\x86\x98\x00\x00\x00\x00", 7) == 0 &&
                  std::memcmp(reinterpret_cast<const void*>(0xB3E023), "\xA1\x34\x17\xAF\x02", 5) == 0 &&
                  std::memcmp(reinterpret_cast<const void*>(0xEAA215), "\x8B\x1D\x30\x17\xAF\x02", 6) == 0,
              "Smoke: smoke screen radius / age / active fields and the global list (0x2AF1730) as grenades.cpp reads them");

        // Limp bodies (limp.cpp): your game's soldier health is ClientSoldierEntity's slot +0xD8 (0x98E950:
        // [soldier+0xD4] +0x18), as the server soldier's (0xAD4070) is.
        Check(*reinterpret_cast<const uint32_t*>(0x27A09B8 + 0xD8) == 0x98E950 &&
                  std::memcmp(reinterpret_cast<const void*>(0x98E950),
                              "\x8B\x81\xD4\x00\x00\x00\x85\xC0\x74\x04\xD9\x40\x18\xC3", 14) == 0 &&
                  *reinterpret_cast<const uint32_t*>(0x27BA820 + 0xD8) == 0xAD4070,
              "Limp bodies: client and server soldier health at vtable +0xD8 (0x98E950 / 0xAD4070)");

        const auto* localLink = reinterpret_cast<const uint8_t*>(0x8A69A1);
        Check(localLink[0] == 0xB0 && localLink[1] == 0x01 && localLink[2] == 0x90 && localLink[3] == 0x88 &&
                  localLink[4] == 0x86,
              "LocalClientLink: setne al replaced with mov al, 1; nop before the store to client+0x1F2");
    }
    {
        auto* dispatch = reinterpret_cast<const uint8_t*>(0x5925C0);
        auto* traceDetour = dispatch + 5 + *reinterpret_cast<const int32_t*>(dispatch + 1);
        VirtualQuery(traceDetour, &mbi, sizeof(mbi));
        Check(dispatch[0] == 0xE9 && mbi.AllocationBase == dll, "message hook: MessageManager::dispatch jumps into the DLL");

        auto* disconnect = reinterpret_cast<const uint8_t*>(0x873E80);
        auto* disconnectDetour = disconnect + 5 + *reinterpret_cast<const int32_t*>(disconnect + 1);
        VirtualQuery(disconnectDetour, &mbi, sizeof(mbi));
        Check(disconnect[0] == 0xE9 && mbi.AllocationBase == dll,
              "TraceMessages: ClientConnection::onDisconnected jumps into the DLL");

        auto* peerDisconnect = reinterpret_cast<const uint8_t*>(0x61FE90);
        auto* peerDetour = peerDisconnect + 5 + *reinterpret_cast<const int32_t*>(peerDisconnect + 1);
        VirtualQuery(peerDetour, &mbi, sizeof(mbi));
        Check(peerDisconnect[0] == 0xE9 && mbi.AllocationBase == dll,
              "TraceMessages: NetPeer::disconnect jumps into the DLL");

        auto* notify = reinterpret_cast<const uint8_t*>(0x613830);
        auto* notifyDetour = notify + 5 + *reinterpret_cast<const int32_t*>(notify + 1);
        VirtualQuery(notifyDetour, &mbi, sizeof(mbi));
        Check(notify[0] == 0xE9 && mbi.AllocationBase == dll,
              "TraceMessages: transport state notify jumps into the DLL");
    }

    {
        // Bot default unlocks: ServerPlayer+0x524 is a vector with 32 inline slots at +0x538 (ServerPlayer ctor
        // 0xB42B9D). Fill all 32 through the DLL and check nothing outside the slots changes (the engine's insert
        // at position == end memmoves -4 bytes, which is what crashed the game on F6).
        using AddUnlockFn = int(__cdecl*)(void* player, void* unlock);
        auto addUnlock = reinterpret_cast<AddUnlockFn>(GetProcAddress(dll, MAKEINTRESOURCEA(200)));
        static uint8_t player[0x700];
        std::memset(player, 0xAB, sizeof(player));
        auto field = [](uint8_t* base, int offset) { return reinterpret_cast<uintptr_t*>(base + offset); };
        uintptr_t inlineSlots = reinterpret_cast<uintptr_t>(player + 0x538);
        *field(player, 0x524) = inlineSlots;         // begin
        *field(player, 0x528) = inlineSlots;         // end
        *field(player, 0x52C) = inlineSlots + 0x80;  // capacity end
        *field(player, 0x530) = 0;                   // allocator arena (lazy)
        *field(player, 0x534) = inlineSlots;         // inline buffer
        bool ok = addUnlock != nullptr;
        for (uintptr_t i = 0; ok && i < 32; ++i)
            ok = addUnlock(player, reinterpret_cast<void*>(0x10000 + i)) == 1;
        for (uintptr_t i = 0; ok && i < 32; ++i)
            ok = *field(player, 0x538 + static_cast<int>(i) * 4) == 0x10000 + i;
        ok = ok && *field(player, 0x524) == inlineSlots && *field(player, 0x528) == inlineSlots + 0x80 &&
             *field(player, 0x534) == inlineSlots && player[0x520] == 0xAB && player[0x5B8] == 0xAB &&
             player[0x6FF] == 0xAB;
        Check(ok, "AddPlayerUnlock: 32 unlocks land in the inline slots, end advances, nothing else touched");
        // A fault inside a guarded engine call is caught and logged (address, registers, stack), not fatal.
        Check(addUnlock && addUnlock(reinterpret_cast<void*>(0x10), nullptr) == 0,
              "guarded call on a bad pointer: fault logged and survived");
    }

    std::printf("hook checks done; testing EntryInput through engine code\n");
    TestEntryInput();
    TestLevelChangeMessages();

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
