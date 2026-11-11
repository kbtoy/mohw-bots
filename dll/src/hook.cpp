#include "hook.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bots.h"
#include "engine.h"
#include "log.h"

namespace {

// Server::update begins with four position-independent instructions (verified by signatures.inc):
//   56                push esi
//   57                push edi
//   8B F1             mov  esi, ecx
//   8D BE B8 00 00 00 lea  edi, [esi+0xB8]
constexpr size_t kStolen = 10;

eng::ServerUpdateFn g_original = nullptr;

// thiscall target, fastcall detour: ecx = this, edx unused, args on the stack, callee cleans up.
bool __fastcall ServerUpdateDetour(void* server, void* /*edx*/, void* updateInfo)
{
    bool result = g_original(server, updateInfo);
    bots::OnServerUpdate(server, updateInfo);
    return result;
}

void WriteJump(uint8_t* at, const void* to)
{
    at[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(at + 5));
    std::memcpy(at + 1, &rel, sizeof(rel));
}

}  // namespace

void* InlineHook(void* targetAddress, size_t stolen, const void* detour)
{
    auto* target = static_cast<uint8_t*>(targetAddress);
    auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline)
    {
        Log("hook: VirtualAlloc failed (%lu)", GetLastError());
        return nullptr;
    }
    std::memcpy(trampoline, target, stolen);
    WriteJump(trampoline + stolen, target + stolen);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("hook: VirtualProtect failed (%lu)", GetLastError());
        return nullptr;
    }
    WriteJump(target, detour);
    std::memset(target + 5, 0x90, stolen - 5);
    VirtualProtect(target, stolen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return trampoline;
}

bool InstallServerUpdateHook()
{
    void* target = reinterpret_cast<void*>(eng::Address("Server_update"));
    void* trampoline = target ? InlineHook(target, kStolen, reinterpret_cast<const void*>(&ServerUpdateDetour)) : nullptr;
    if (!trampoline)
        return false;
    g_original = reinterpret_cast<eng::ServerUpdateFn>(trampoline);
    Log("hook: Server::update @ %p -> detour %p (trampoline %p)", target, &ServerUpdateDetour, trampoline);
    return true;
}

bool PatchStartupArgs(const char* extraArgs)
{
    static char args[1024];
    std::snprintf(args, sizeof(args), "-webmode spoffline %s", extraArgs);

    // 68 imm32 = push "-webmode spoffline"; only the immediate changes (signature-checked).
    auto* push = reinterpret_cast<uint8_t*>(eng::Address("Startup_pushOfflineArgs"));
    if (!push)
        return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(push, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("startup args: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    const char* operand = args;
    std::memcpy(push + 1, &operand, sizeof(operand));
    VirtualProtect(push, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), push, 5);
    Log("startup args: offline mode will start with \"%s\" (applies only when launched with no arguments)", args);
    return true;
}

bool PatchDisableOrigin()
{
    // mov al, [eax+0x6C]  (8A 40 6C)  ->  xor al, al; nop  (32 C0 90); signature-checked.
    auto* read = reinterpret_cast<uint8_t*>(eng::Address("Origin_readEnableOrigin"));
    if (!read)
        return false;
    const uint8_t patch[] = {0x32, 0xC0, 0x90};
    DWORD oldProtect = 0;
    if (!VirtualProtect(read, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("disable origin: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    std::memcpy(read, patch, sizeof(patch));
    VirtualProtect(read, sizeof(patch), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), read, sizeof(patch));
    Log("disable origin: Origin component will treat EnableOrigin as false (no Origin SDK / EA app)");
    return true;
}

bool PatchForceLocalServerBackend()
{
    // jne 0xD25831 (75 70)  ->  jmp 0xD25831 (EB 70): always `mov edi, 3` = Backend_Local; signature-checked.
    auto* select = reinterpret_cast<uint8_t*>(eng::Address("ServerBackend_select"));
    if (!select)
        return false;
    uint8_t* opcode = select + 6;
    DWORD oldProtect = 0;
    if (!VirtualProtect(opcode, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("force local backend: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    *opcode = 0xEB;
    VirtualProtect(opcode, 1, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), opcode, 1);
    Log("force local backend: every server uses Backend_Local (Blaze's factory isn't registered offline)");
    return true;
}

bool PatchForceServerAnimation()
{
    // test al, al; jne 0xAAA078 (84 C0 0F 85 rel32) twice -> test al, al; 6 x nop; signature-checked.
    uint8_t* sites[2] = {reinterpret_cast<uint8_t*>(eng::Address("ServerAnim_skipIfSinglePlayer")),
                         reinterpret_cast<uint8_t*>(eng::Address("ServerAnim_skipIfCoop"))};
    if (!sites[0] || !sites[1])
        return false;
    for (uint8_t* site : sites)
    {
        uint8_t* jne = site + 2;
        DWORD oldProtect = 0;
        if (!VirtualProtect(jne, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            Log("force server animation: VirtualProtect failed (%lu)", GetLastError());
            return false;
        }
        std::memset(jne, 0x90, 6);
        VirtualProtect(jne, 6, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), jne, 6);
    }
    Log("force server animation: the server creates its animation manager whatever the game type (soldiers' "
        "server-side hit shapes follow them)");
    return true;
}

bool PatchBotBulletsAsPlayers()
{
    // cmp byte ptr [esi+0x354], 0; je +0x2C (74 2C) -> jmp (EB 2C): the damage sum never applies an AI shooter's
    // weapon multiplier; signature-checked. One function serves the server and your game, so it covers both.
    auto* site = reinterpret_cast<uint8_t*>(eng::Address("Damage_aiWeaponMultiplier"));
    if (!site)
        return false;
    uint8_t* je = site + 7;
    DWORD oldProtect = 0;
    if (!VirtualProtect(je, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("bot bullets as players: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    *je = 0xEB;
    VirtualProtect(je, 1, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), je, 1);
    Log("bot bullets as players: a bot's bullets do a player's damage (no campaign AI weapon multiplier, which cut "
        "team 2's to about 40%%)");
    return true;
}

namespace {

bool WriteCode(uint8_t* at, const void* bytes, size_t count)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("code patch at %p: VirtualProtect failed (%lu)", at, GetLastError());
        return false;
    }
    std::memcpy(at, bytes, count);
    VirtualProtect(at, count, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), at, count);
    return true;
}

}  // namespace

bool PatchBotHealthFromServer()
{
    auto* handlesHit = reinterpret_cast<uint8_t*>(eng::Address("ClientSoldier_handlesHit_aiVictim"));
    auto* ownEstimate = reinterpret_cast<uint8_t*>(eng::Address("ClientHealth_usesOwnEstimate"));
    auto* hitInfo = reinterpret_cast<uint8_t*>(eng::Address("ClientHealth_ghostHitInfoSkipAI"));
    if (!handlesHit || !ownEstimate || !hitInfo)
        return false;
    const uint8_t noHit = 0xB3;                       // jne 0x962E2C -> jne 0x962DD4 (xor al, al; ret 8)
    const uint8_t alwaysServer[] = {0x33, 0xC0, 0xC3};  // xor eax, eax; ret
    const uint8_t keepHitInfo[] = {0x90, 0x90};       // jne over the copy -> nops
    if (!WriteCode(handlesHit + 8, &noHit, 1) || !WriteCode(ownEstimate, alwaysServer, sizeof(alwaysServer)) ||
        !WriteCode(hitInfo + 7, keepHitInfo, sizeof(keepHitInfo)))
        return false;
    Log("bot health from server: your game follows the server's health for bots as it does for a remote player "
        "(no own estimate, so no bot dead only on your screen)");
    return true;
}

bool PatchLocalClientLink()
{
    // setne al (0F 95 C0) -> mov al, 1; nop (B0 01 90): client+0x1F2 is always 1; signature-checked.
    auto* read = reinterpret_cast<uint8_t*>(eng::Address("ClientStartMultiplayer_readLocalLink"));
    if (!read)
        return false;
    uint8_t* setne = read + 7;
    DWORD oldProtect = 0;
    if (!VirtualProtect(setne, 3, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("local client link: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    setne[0] = 0xB0;
    setne[1] = 0x01;
    setne[2] = 0x90;
    VirtualProtect(setne, 3, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), setne, 3);
    Log("local client link: multiplayer starts connect to the in-process server directly (mode 2), not over UDP");
    return true;
}

bool PatchChallengeRetries(int retries)
{
    if (retries < 1 || retries > 127)  // the limit is a sign-extended imm8
    {
        Log("challenge retries: %d is out of range (1..127); keeping the game's 15", retries);
        return false;
    }
    const char* sites[] = {"Transport_challengeRetryLimit1", "Transport_challengeRetryLimit2"};
    for (const char* site : sites)
    {
        // cmp dword ptr [esi+0xA0], imm8: the limit is the last byte of the 7-byte instruction; signature-checked.
        auto* cmp = reinterpret_cast<uint8_t*>(eng::Address(site));
        if (!cmp)
            return false;
        uint8_t* limit = cmp + 6;
        DWORD oldProtect = 0;
        if (!VirtualProtect(limit, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            Log("challenge retries: VirtualProtect failed (%lu)", GetLastError());
            return false;
        }
        *limit = static_cast<uint8_t>(retries);
        VirtualProtect(limit, 1, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), limit, 1);
    }
    Log("challenge retries: a connecting client keeps knocking for %.1f s (game default 7.5 s) before NoReply",
        retries * 0.5);
    return true;
}
