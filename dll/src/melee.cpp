#include "melee.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "engine.h"
#include "hook.h"
#include "log.h"

namespace melee {
namespace {

constexpr int kClientSoldier_Melee = 0x420;  // its ClientMeleeComponent (0x958D60 calls 0x981870 on it)
constexpr int kClientMelee_Target = 0x234;   // weak pointer to the soldier in your sights: [[+0x234]] - 4
constexpr int kServerSoldier_Player = 0x10C;
constexpr uintptr_t kServerMeleeManager = 0x2AF31B0;  // static object; ecx of 0xB844D0 and 0xB73D40

using InvolvedFn = bool(__thiscall*)(void* manager, void* soldier);
using StartFn = int(__thiscall*)(void* manager, void* attacker, void* victim);

InvolvedFn g_involved = nullptr;
StartFn g_start = nullptr;
bool g_on = false;
bool g_targetLogged = false;
bool g_botTargetLogged = false;

void DescribeServerSoldier(void* soldier, char* out, size_t size)
{
    void* player = nullptr;
    uint8_t ai = 0;
    int id = -1;
    if (eng::ReadPtr(eng::At(soldier, kServerSoldier_Player), &player) && player &&
        eng::ReadByte(eng::At(player, eng::kPlayer_IsAI), &ai) && eng::GetPlayerId(player, &id))
        std::snprintf(out, size, "%s (player id %d)", ai ? "a bot" : "you", id);
    else
        std::snprintf(out, size, "soldier %p", soldier);
}

int __fastcall StartDetour(void* manager, void* /*edx*/, void* attacker, void* victim)
{
    char a[48], v[48];
    DescribeServerSoldier(attacker, a, sizeof(a));
    DescribeServerSoldier(victim, v, sizeof(v));
    Log("melee: the server starts a stealth kill: %s on %s", a, v);
    return g_start(manager, attacker, victim);
}

bool PatchAiVictimCheck()
{
    // call victim->isAI; test al, al; je accept (74 21) -> jmp accept (EB 21); signature-checked.
    auto* site = reinterpret_cast<uint8_t*>(eng::Address("ClientMelee_aiVictimCheck"));
    if (!site)
        return false;
    uint8_t* je = site + 10;
    DWORD oldProtect = 0;
    if (!VirtualProtect(je, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("melee: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    *je = 0xEB;
    VirtualProtect(je, 1, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), je, 1);
    return true;
}

}  // namespace

void Init()
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    if (GetPrivateProfileIntW(L"bots", L"StealthMeleeOnBots", 1, path) == 0)
    {
        Log("melee: stealth kills on bots off (StealthMeleeOnBots=0)");
        return;
    }
    g_involved = reinterpret_cast<InvolvedFn>(eng::Address("ServerMelee_isInvolved"));
    void* start = reinterpret_cast<void*>(eng::Address("ServerMelee_start"));
    if (!g_involved || !start || !PatchAiVictimCheck())
    {
        Log("melee: stealth kills on bots NOT enabled (a signature didn't match)");
        return;
    }
    // push ebp; mov ebp, esp; and esp, -16: 6 position-independent bytes (signature-checked).
    if (void* trampoline = InlineHook(start, 6, reinterpret_cast<const void*>(&StartDetour)))
        g_start = reinterpret_cast<StartFn>(trampoline);
    g_on = true;
    Log("melee: your melee can stealth-kill a bot as it does a player; a bot in one has a neutral input");
}

void ClientTick()
{
    if (!g_on || g_botTargetLogged)
        return;
    void* local = nullptr;
    void* soldier = nullptr;
    void* component = nullptr;
    void* weak = nullptr;
    void* object = nullptr;
    if (!eng::ClientLocalPlayer(&local) || !local || !eng::ClientSoldierOf(local, &soldier) || !soldier ||
        !eng::ReadPtr(eng::At(soldier, kClientSoldier_Melee), &component) || !component ||
        !eng::ReadPtr(eng::At(component, kClientMelee_Target), &weak) || !weak || !eng::ReadPtr(weak, &object) || !object)
        return;
    void* target = static_cast<char*>(object) - 4;
    void* player = nullptr;
    uint8_t ai = 0;
    if (!eng::ReadPtr(eng::At(target, eng::kClientSoldier_Player), &player) || !player ||
        !eng::ReadByte(eng::At(player, eng::kPlayer_IsAI), &ai))
        return;
    if (ai)
    {
        g_botTargetLogged = true;
        Log("melee: your melee can target a bot (the first one seen in your sights)");
    }
    else if (!g_targetLogged)
    {
        g_targetLogged = true;
        Log("melee: your melee's target is a human player's soldier");
    }
}

bool Involved(void* serverSoldier)
{
    if (!g_involved)
        return false;
    __try
    {
        return g_involved(reinterpret_cast<void*>(kServerMeleeManager), serverSoldier);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

}  // namespace melee
