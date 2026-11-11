#include "engine.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <iterator>

#include "log.h"

namespace eng {
namespace {

struct Signature
{
    const char* name;
    uintptr_t address;
    uint8_t count;
    uint8_t bytes[16];
};

#define SIG(name, address, count, ...) {#name, address, count, {__VA_ARGS__}},
const Signature kSignatures[] = {
#include "signatures.inc"
};
#undef SIG

namespace addr {
#define SIG(name, address, ...) constexpr uintptr_t name = address;
#include "signatures.inc"
#undef SIG
}  // namespace addr

// Engine calling conventions (all thiscall unless noted), verified in re/RE_NOTES.md.
using CreatePlayerFn = void*(__thiscall*)(void* manager, void* connection, int id, int isSpectator);
using SetPlayerNameFn = void(__thiscall*)(void* manager, void* player, const char* name);
using SetIntFn = void(__thiscall*)(void* self, int value);
using SetPtrFn = void(__thiscall*)(void* self, void* value);
using GetLoadoutStateFn = void*(__cdecl*)(void* player);
using CtorFn = void*(__thiscall*)(void* self);
using DtorFn = void(__thiscall*)(void* self);
using DispatchFn = void(__thiscall*)(void* context, void* message);
using SpawnAtFn = void(__thiscall*)(void* extent, void* player, const float* transform, int a4, int notify);
using GetTransformFn = const float*(__thiscall*)(void* iface);
using GetContainerFn = void*(__thiscall*)(void* manager, void* typeInfo);
using InsertFn = void(__thiscall*)(void* vector, void* position, void* const* value);

// Logs where an engine call faulted: the instruction, the access, the registers, and the game-code return addresses
// on the faulting stack (look them up with tools/disfunc.py or re/labels.json).
int Fault(const char* what, EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    const CONTEXT* c = info->ContextRecord;
    bool access = record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2;
    Log("ENGINE FAULT in %s (exception 0x%08X at %08X%s%08X) eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
        "ebp=%08X esp=%08X",
        what, record->ExceptionCode, static_cast<unsigned>(reinterpret_cast<uintptr_t>(record->ExceptionAddress)),
        access ? (record->ExceptionInformation[0] ? ", writing " : ", reading ") : ", ",
        access ? static_cast<unsigned>(record->ExceptionInformation[1]) : 0u, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi,
        c->Edi, c->Ebp, c->Esp);
    char line[256] = "";
    size_t used = 0;
    const uintptr_t* stack = reinterpret_cast<const uintptr_t*>(static_cast<uintptr_t>(c->Esp));
    for (int i = 0, found = 0; i < 512 && found < 16 && used + 12 < sizeof(line); ++i)
    {
        void* value = nullptr;
        if (!ProbePtr(stack + i, &value))
            break;
        if (LooksLikeReturnAddress(reinterpret_cast<uintptr_t>(value)))
        {
            used += std::snprintf(line + used, sizeof(line) - used, " %08X",
                                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(value)));
            ++found;
        }
    }
    Log("  fault stack (game return addresses):%s", line);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Runs the body under SEH; variadic so commas inside the body can't split the macro arguments.
#define GUARDED(what, ...)                                           \
    __try                                                            \
    {                                                                \
        __VA_ARGS__;                                                 \
        return true;                                                 \
    }                                                                \
    __except (Fault(what, GetExceptionInformation()))                \
    {                                                                \
        return false;                                                \
    }

}  // namespace

bool ReadBytes(const void* address, void* out, size_t count) { GUARDED("ReadBytes", std::memcpy(out, address, count)) }
bool WriteBytes(void* address, const void* data, size_t count) { GUARDED("WriteBytes", std::memcpy(address, data, count)) }

bool VerifySignatures()
{
    bool ok = true;
    for (const Signature& s : kSignatures)
    {
        uint8_t actual[16] = {};
        if (!ReadBytes(reinterpret_cast<const void*>(s.address), actual, s.count) ||
            std::memcmp(actual, s.bytes, s.count) != 0)
        {
            Log("signature MISMATCH %-40s @ 0x%08X (found %02X %02X %02X %02X ...)", s.name,
                static_cast<unsigned>(s.address), actual[0], actual[1], actual[2], actual[3]);
            ok = false;
        }
    }
    Log("signature check: %s (%u functions)", ok ? "all match" : "FAILED", static_cast<unsigned>(std::size(kSignatures)));
    return ok;
}

uintptr_t Address(const char* signatureName)
{
    for (const Signature& s : kSignatures)
        if (std::strcmp(s.name, signatureName) == 0)
            return s.address;
    return 0;
}

bool ReadU32(const void* address, uint32_t* out) { GUARDED("ReadU32", *out = *static_cast<const volatile uint32_t*>(address)) }
bool ReadPtr(const void* address, void** out) { GUARDED("ReadPtr", *out = *static_cast<void* const volatile*>(address)) }
bool ReadByte(const void* address, uint8_t* out) { GUARDED("ReadByte", *out = *static_cast<const volatile uint8_t*>(address)) }
bool ReadFloat(const void* address, float* out) { GUARDED("ReadFloat", *out = *static_cast<const volatile float*>(address)) }

bool ProbePtr(const void* address, void** out)
{
    __try
    {
        *out = *static_cast<void* const volatile*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ProbeByte(const void* address, uint8_t* out)
{
    __try
    {
        *out = *static_cast<const volatile uint8_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool LooksLikeCode(uintptr_t address)
{
    return address >= 0x401000 && address < 0x273B000;  // .text and ctr both hold code
}

bool LooksLikeReturnAddress(uintptr_t address)
{
    if (!LooksLikeCode(address))
        return false;
    uint8_t op = 0;
    if (ProbeByte(reinterpret_cast<const void*>(address - 5), &op) && op == 0xE8)  // call rel32
        return true;
    static const int kLengths[] = {2, 3, 4, 6, 7};  // FF /2: call reg, [reg], [reg+d8], [reg+d32], [abs], +SIB
    for (int length : kLengths)
    {
        uint8_t modrm = 0;
        if (ProbeByte(reinterpret_cast<const void*>(address - length), &op) && op == 0xFF &&
            ProbeByte(reinterpret_cast<const void*>(address - length + 1), &modrm) && ((modrm >> 3) & 7) == 2)
            return true;
    }
    return false;
}

void* ServerGameContext()
{
    void* ctx = nullptr;
    ReadPtr(reinterpret_cast<const void*>(kServerGameContextPtr), &ctx);
    return ctx;
}

bool GetSettingsContainer(uintptr_t typeInfo, void** outContainer)
{
    *outContainer = nullptr;
    void* manager = nullptr;
    if (!ReadPtr(reinterpret_cast<const void*>(kSettingsManagerPtr), &manager) || !manager)
        return false;
    GUARDED("SettingsManager::getContainer",
            *outContainer = reinterpret_cast<GetContainerFn>(addr::SettingsManager_getContainer)(
                manager, reinterpret_cast<void*>(typeInfo)))
}

bool CreatePlayer(void* playerManager, void** outPlayer)
{
    GUARDED("ServerPlayerManager::createPlayer",
            *outPlayer = reinterpret_cast<CreatePlayerFn>(addr::ServerPlayerManager_createPlayer)(playerManager, nullptr, -1, 0))
}

bool SetPlayerName(void* playerManager, void* player, const char* name)
{
    GUARDED("ServerPlayerManager::setPlayerName",
            reinterpret_cast<SetPlayerNameFn>(addr::ServerPlayerManager_setPlayerName)(playerManager, player, name))
}

bool SetIsAIPlayer(void* player, bool isAI)
{
    GUARDED("ServerPlayer::setIsAIPlayer", reinterpret_cast<SetIntFn>(addr::ServerPlayer_setIsAIPlayer)(player, isAI ? 1 : 0))
}

bool SetTeam(void* player, int team)
{
    GUARDED("ServerPlayer::setTeam", reinterpret_cast<SetIntFn>(addr::ServerPlayer_setTeam)(player, team))
}

bool SetSelectedSoldierAsset(void* player, void* asset)
{
    GUARDED("ServerPlayer::setSelectedSoldierAsset",
            reinterpret_cast<SetPtrFn>(addr::ServerPlayer_setSelectedSoldierAsset)(player, asset))
}

bool SetInput(void* player, void* input)
{
    GUARDED("ServerPlayer::setInput", reinterpret_cast<SetPtrFn>(addr::ServerPlayer_setInput)(player, input))
}

bool GetPlayerLoadoutState(void* player, void** outState)
{
    GUARDED("getPlayerLoadoutState", *outState = reinterpret_cast<GetLoadoutStateFn>(addr::getPlayerLoadoutState)(player))
}

bool SetKitIndex(void* loadoutState, int index)
{
    GUARDED("PlayerLoadoutState::setKitIndex",
            reinterpret_cast<SetIntFn>(addr::PlayerLoadoutState_setKitIndex)(loadoutState, index))
}

// Same as the game's own NetworkPlayerSelectedUnlockAssetsMessage handler (0xB41C14): append in place while there's
// room (the first 32 live in inline storage at +0x538), call the engine's insert only when full. That insert's
// in-place path assumes position < end; at position == end it memmoves (end - 4 - position) = -4 bytes and wrecks
// the heap, so it must only ever see a full vector (its growth path handles the end correctly).
bool AddPlayerUnlock(void* player, void* unlock)
{
    void** vector = reinterpret_cast<void**>(static_cast<char*>(player) + kPlayer_Unlocks);
    GUARDED("UnlockVector::push_back",
            void** end = static_cast<void**>(vector[1]);
            if (end < static_cast<void**>(vector[2])) {
                *end = unlock;
                vector[1] = end + 1;
            } else {
                reinterpret_cast<InsertFn>(addr::UnlockVector_insert)(vector, end, &unlock);
            })
}

bool ApplyDefaultLoadout(void* player, void* kit)
{
    using HandlerFn = void(__thiscall*)(void* player, void* message);
    alignas(16) uint8_t message[0x80] = {};  // NetworkPlayerSelectedUnlockAssetsMessage is 0x60 bytes
    void** kitField = reinterpret_cast<void**>(message + 0x38);
    GUARDED("default loadout (ServerPlayer selected-unlocks handler)",
            reinterpret_cast<CtorFn>(addr::UnlockAssetsMessage_ctor)(message);
            *kitField = kit;
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<char*>(kit) + 4));  // as the game does
            reinterpret_cast<HandlerFn>(addr::ServerPlayer_onSelectedUnlockAssets)(player, message);
            reinterpret_cast<DtorFn>(addr::FbArray_dtor)(message + 0x4C);
            reinterpret_cast<DtorFn>(addr::FbArray_dtor)(message + 0x48);
            if (*kitField && !(reinterpret_cast<uintptr_t>(*kitField) & 1))
                reinterpret_cast<DtorFn>(addr::RefCounted_release)(*kitField);
            reinterpret_cast<DtorFn>(addr::NetworkMessage_dtor)(message))
}

bool StepSoldier(void* soldier, bool force, bool* stepped)
{
    using ShouldFn = bool(__thiscall*)(void* soldier);
    using StepFn = void(__thiscall*)(void* soldier, int entryIndex);
    *stepped = false;
    bool* out = stepped;
    // Through the soldier's own vtable (+0x148 / +0x14C; ServerSoldierEntity's are 0xB64800 / 0xB64890, both
    // signature-checked) in case a subclass overrides them.
    void** vtable = nullptr;
    void* should = nullptr;
    void* step = nullptr;
    if (!ProbePtr(soldier, reinterpret_cast<void**>(&vtable)) || !vtable || !ProbePtr(vtable + 0x148 / 4, &should) ||
        !ProbePtr(vtable + 0x14C / 4, &step) || !LooksLikeCode(reinterpret_cast<uintptr_t>(should)) ||
        !LooksLikeCode(reinterpret_cast<uintptr_t>(step)))
        return false;
    GUARDED("ServerSoldierEntity::singleStepEntry",
            if (force || reinterpret_cast<ShouldFn>(should)(soldier)) {
                reinterpret_cast<StepFn>(step)(soldier, 0);
                *out = true;
            })
}

volatile LONG g_rayCount = 0;

uint32_t TakeRayCount()
{
    return static_cast<uint32_t>(InterlockedExchange(&g_rayCount, 0));
}

bool RayBlocked(const float from[3], const float to[3], uint32_t flags, bool* blocked)
{
    float fraction = 1.0f;
    return RayHit(from, to, flags, blocked, &fraction);
}

bool RayHit(const float from[3], const float to[3], uint32_t flags, bool* blocked, float* fraction)
{
    InterlockedIncrement(&g_rayCount);
    using RayFn = bool(__thiscall*)(void* physics, const char* ident, int mode, const void* from, const void* to,
                                    void* hit, uint32_t flags, void* excluded);
    void* physics = nullptr;
    if (!ProbePtr(reinterpret_cast<const void*>(kServerPhysicsPtr), &physics) || !physics)
        return false;
    alignas(16) float a[4] = {from[0], from[1], from[2], 0.0f};
    alignas(16) float b[4] = {to[0], to[1], to[2], 0.0f};
    alignas(16) uint8_t hit[0x100] = {};
    std::memset(hit + 0x30, 0xFF, 8);  // as the spawn chooser initialises its RayCastHit (0xEE4788)
    bool result = false;
    GUARDED("ServerPhysics::rayQuery",
            result = reinterpret_cast<RayFn>(addr::ServerPhysics_rayQuery)(physics, "mohw-bots", 1, a, b, hit, flags,
                                                                          nullptr);
            *blocked = result;
            *fraction = result ? *reinterpret_cast<const float*>(hit + 0x30) : 1.0f)
}

bool GetCharacterPose(void* soldier, int* pose, int* targetPose, int* state)
{
    void* physics = nullptr;
    if (!soldier || !ReadPtr(At(soldier, kSoldier_CharacterPhysics), &physics) || !physics)
        return false;
    uint32_t value = 0;
    if (pose && (!ReadU32(At(physics, kCharPhysics_Pose), &value) || value > kPoseProne))
        return false;
    if (pose)
        *pose = static_cast<int>(value);
    if (targetPose && (!ReadU32(At(physics, kCharPhysics_TargetPose), &value) || value > kPoseProne))
        return false;
    if (targetPose)
        *targetPose = static_cast<int>(value);
    if (state)
    {
        void* current = nullptr;
        if (!ReadPtr(At(physics, kCharPhysics_State), &current) || !current ||
            !ReadU32(At(current, kCharState_Type), &value) || value > kStateRappeling)
            return false;
        *state = static_cast<int>(value);
    }
    return true;
}

float WaterDepth(const float feet[3])
{
    // 8 m down: probes from the ledge 4.2 m over the MP_03 bay used to stop short of the water and read dry.
    constexpr float kAbove = 2.0f, kBelow = 8.0f;
    float from[3] = {feet[0], feet[1] + kAbove, feet[2]};
    float to[3] = {feet[0], feet[1] - kBelow, feet[2]};
    bool wet = false, dry = false;
    float wetAt = 1.0f, dryAt = 1.0f;
    if (!RayHit(from, to, kRay_DontCheckRagdoll | kRay_DontCheckCharacter, &wet, &wetAt) || !wet ||
        !RayHit(from, to, kRay_DontCheckWater | kRay_DontCheckRagdoll | kRay_DontCheckCharacter, &dry, &dryAt))
        return 0.0f;
    float depth = (dryAt - wetAt) * (kAbove + kBelow);  // ground below the reach: at least down to it
    return depth > 0.0f ? depth : 0.0f;
}

const char* PoseName(int pose)
{
    static const char* const kNames[] = {"standing", "crouched", "prone"};
    return pose >= 0 && pose <= kPoseProne ? kNames[pose] : "?";
}

const char* StateName(int state)
{
    static const char* const kNames[] = {"on ground", "jumping",  "in air",          "climbing",
                                         "falling",   "parachute", "swimming",       "animation-controlled",
                                         "sliding",   "sprint-sliding", "rappeling"};
    return state >= 0 && state <= kStateRappeling ? kNames[state] : "?";
}

bool IsKindOf(void* object, uintptr_t typeInfo)
{
    using GetTypeFn = const void*(__thiscall*)(void* self);
    const void* type = nullptr;
    void** vtable = nullptr;
    void* getType = nullptr;
    if (!object || !ProbePtr(object, reinterpret_cast<void**>(&vtable)) || !vtable || !ProbePtr(vtable, &getType) ||
        !LooksLikeCode(reinterpret_cast<uintptr_t>(getType)))
        return false;
    __try
    {
        type = reinterpret_cast<GetTypeFn>(getType)(object);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    uint8_t lo[2] = {}, hi[2] = {}, id[2] = {};
    if (!type || !ProbeByte(At(type, kTypeInfo_ClassId), &id[0]) || !ProbeByte(At(type, kTypeInfo_ClassId + 1), &id[1]))
        return false;
    const void* target = reinterpret_cast<const void*>(typeInfo);
    ProbeByte(At(target, kTypeInfo_ClassId), &lo[0]);
    ProbeByte(At(target, kTypeInfo_ClassId + 1), &lo[1]);
    ProbeByte(At(target, kTypeInfo_LastSubclassId), &hi[0]);
    ProbeByte(At(target, kTypeInfo_LastSubclassId + 1), &hi[1]);
    unsigned classId = id[0] | (id[1] << 8);
    unsigned first = lo[0] | (lo[1] << 8);
    unsigned last = hi[0] | (hi[1] << 8);
    return classId - first <= last - first;  // unsigned, as the game compares (sub; sub; cmp; ja)
}

}  // namespace eng

// Test hook for tools/smoketest/imagetest (exported in xinput1_3.def; the game never calls it).
extern "C" int MohwBots_AddPlayerUnlock(void* player, void* unlock)
{
    return eng::AddPlayerUnlock(player, unlock) ? 1 : 0;
}

namespace eng {

bool ConstructEntryInput(void* buffer)
{
    GUARDED("EntryInput::EntryInput", reinterpret_cast<CtorFn>(addr::EntryInput_ctor)(buffer))
}

bool DestroyEntryInput(void* input)
{
    GUARDED("EntryInput::~EntryInput", reinterpret_cast<DtorFn>(addr::EntryInput_dtor)(input))
}

bool SetAuthoritativeAiming(void* input, bool on)
{
    GUARDED("EntryInput::setAuthoritativeAiming",
            reinterpret_cast<SetIntFn>(addr::EntryInput_setAuthoritativeAiming)(input, on ? 1 : 0))
}

bool ForceRespawn(void* serverGameContext, void* player)
{
    alignas(16) unsigned char message[0x40] = {};  // ServerPlayerForceRespawnMessage is 0x30 bytes
    GUARDED("ServerPlayerForceRespawnMessage", {
        reinterpret_cast<CtorFn>(addr::ServerPlayerForceRespawnMessage_ctor)(message);
        *reinterpret_cast<void**>(message + 0x28) = player;
        reinterpret_cast<DispatchFn>(addr::GameContext_dispatchMessage)(serverGameContext, message);
        reinterpret_cast<DtorFn>(addr::Message_dtor)(message);
    })
}

bool DeleteAiPlayer(void* serverGameContext, void* player)
{
    using DeleteFn = void(__thiscall*)(void* manager, void* player);
    void* manager = nullptr;
    if (!serverGameContext || !player || !ProbePtr(At(serverGameContext, kCtx_PlayerManager), &manager) || !manager)
        return false;
    GUARDED("ServerPlayerManager::deletePlayer", {
        reinterpret_cast<DtorFn>(addr::ServerPlayer_releaseLoadout)(player);
        reinterpret_cast<DeleteFn>(addr::ServerPlayerManager_deletePlayer)(manager, player);
    })
}

namespace {

using AssignFn = void*(__thiscall*)(void* cstring, const char* text, size_t length);
using SetOptionFn = void(__thiscall*)(void* setup, const char* criterion, const char* value);
using CopySetupFn = void*(__thiscall*)(void* to, const void* from);
using AllocFn = void*(__thiscall*)(void* arena, size_t size, size_t alignment);
using QueueFn = void(__thiscall*)(void* context, void* message, float delay);

constexpr uintptr_t kServerArena = 0x2A59130;
constexpr uintptr_t kServerLoadLevelMessageVtable = 0x27AD710;  // getType 0x259EB20: ServerLoadLevelMessageBase
constexpr uintptr_t kServerLoadLevelGetType = 0x259EB20;
constexpr int kRequest_Setup = 0x38;      // NetworkRequestLoadLevelMessage.Setup
constexpr int kSetup_Difficulty = 0x08;   // LevelSetup.DifficultyIndex
constexpr int kSetup_ForceReload = 0x18;  // LevelSetup.ForceReloadResources
constexpr int kLoad_Setup = 0x28;
constexpr int kLoad_Flag = 0x40;
constexpr int kSynced_Difficulty = 0x18;  // SyncedGameSettings: what the server's request handler compares the difficulty with

bool ConstructRequest(void* request)
{
    GUARDED("NetworkRequestLoadLevelMessage", reinterpret_cast<CtorFn>(addr::NetworkRequestLoadLevel_ctor)(request))
}

// Builds the setup in `request` (constructed already); separate from the queueing so a fault says which half it was.
bool FillSetup(void* request, const char* levelPath, const char* gameMode, uint32_t difficulty, bool forceReload)
{
    GUARDED("LevelSetup for an in-place level change", {
        char* setup = static_cast<char*>(request) + kRequest_Setup;
        reinterpret_cast<AssignFn>(addr::CString_assign)(setup, levelPath, std::strlen(levelPath));
        if (gameMode && gameMode[0])
            reinterpret_cast<SetOptionFn>(addr::LevelSetup_setInclusionOption)(setup, "GameMode", gameMode);
        *reinterpret_cast<uint32_t*>(setup + kSetup_Difficulty) = difficulty;
        *reinterpret_cast<bool*>(setup + kSetup_ForceReload) = forceReload;
    })
}

bool QueueLoad(void* context, const void* setup)
{
    GUARDED("ServerLoadLevelMessage", {
        void* arena = reinterpret_cast<void*>(kServerArena);
        char* message = static_cast<char*>(reinterpret_cast<AllocFn>(addr::Arena_alloc)(arena, 0x48, 0x10));
        if (!message)
            return false;
        reinterpret_cast<CtorFn>(addr::ServerLoadLevelMessage_ctor)(message);
        *reinterpret_cast<uintptr_t*>(message) = kServerLoadLevelMessageVtable;
        reinterpret_cast<CopySetupFn>(addr::LevelSetup_copy)(message + kLoad_Setup, setup);
        message[kLoad_Flag] = 0;
        reinterpret_cast<QueueFn>(addr::GameContext_queueMessage)(context, message, 0.6f);
    })
}

bool DispatchRoundReset(void* context)
{
    alignas(16) unsigned char message[0x40] = {};
    GUARDED("ServerRoundResetMessage", {
        reinterpret_cast<CtorFn>(addr::ServerRoundResetMessage_ctor)(message);
        reinterpret_cast<DispatchFn>(addr::GameContext_dispatchMessage)(context, message);
        reinterpret_cast<DtorFn>(addr::Message_dtor)(message);
    })
}

bool DestroyRequest(void* request)
{
    GUARDED("NetworkRequestLoadLevelMessage teardown", {
        reinterpret_cast<DtorFn>(addr::LevelSetup_dtor)(static_cast<char*>(request) + kRequest_Setup);
        reinterpret_cast<DtorFn>(addr::NetworkMessage_dtor)(request);
    })
}

}  // namespace

const char* RequestLevelLoad(const char* levelPath, const char* gameMode, bool forceReload)
{
    void* context = ServerGameContext();
    if (!context)
        return "no server";
    void* getType = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(kServerLoadLevelMessageVtable), &getType) ||
        reinterpret_cast<uintptr_t>(getType) != kServerLoadLevelGetType)
        return "the load message's vtable isn't where it should be";
    uint32_t difficulty = 0;
    void* synced = nullptr;
    if (GetSettingsContainer(kTypeInfo_SyncedGameSettings, &synced) && synced)
        ReadU32(At(synced, kSynced_Difficulty), &difficulty);
    void* request = _aligned_malloc(0x60, 16);
    if (!request)
        return "out of memory";
    std::memset(request, 0, 0x60);
    const bool built = ConstructRequest(request);
    const char* failed = nullptr;
    if (!built)
        failed = "building the request failed";
    else if (!FillSetup(request, levelPath, gameMode, difficulty, forceReload))
        failed = "filling the level setup failed";
    else if (!QueueLoad(context, static_cast<char*>(request) + kRequest_Setup))
        failed = "queueing the load failed";
    else if (!DispatchRoundReset(context))
        failed = "the round reset failed (the load is queued)";
    if (built)
        DestroyRequest(request);
    _aligned_free(request);
    return failed;
}

bool ReadServerLoadFlags(void* server, uint8_t flags[6], void** currentLevel)
{
    static const int kOffsets[6] = {0x40, 0x41, 0x42, 0x6C, 0x6D, 0x6E};
    for (int i = 0; i < 6; ++i)
        if (!ReadByte(At(server, kOffsets[i]), &flags[i]))
            return false;
    return ReadPtr(At(server, 0x7C), currentLevel);
}

bool SpawnAt(void* player, const float* transform16)
{
    GUARDED("VeniceServerPlayerExtent::spawnAt", {
        int extentOffset = *reinterpret_cast<const int*>(kPlayerExtentOffset);
        void* extent = static_cast<char*>(player) + extentOffset;
        reinterpret_cast<SpawnAtFn>(addr::PlayerExtent_spawnAt)(extent, player, transform16, 0, 1);
    })
}

bool GetSoldierTransform(void* soldier, const float** outTransform16)
{
    GUARDED("soldier transform", {
        void* iface = static_cast<char*>(soldier) + 0x130;
        GetTransformFn fn = reinterpret_cast<GetTransformFn>((*reinterpret_cast<void***>(iface))[3]);
        *outTransform16 = fn(iface);
    })
}

bool SendScoreEvent(void* player, int statEvent, float points, const char* sid, int visibility, float buddyPoints,
                    const char* buddySid, float buddyDistance)
{
    // ctor(this, player, otherPlayer, supportFlag (+0x68: the support-action collection counts only flagged events),
    // points, &{sidBegin, sidEnd}, bucket, visibility, buddySid (eastl::string* or null = ""), buddyPoints,
    // buddyDistance, requestVO); ret 0x2C. Fields: +0x28 player, +0x30 points, +0x34 sid, +0x38 bucket,
    // +0x3C visibility, +0x40 StatEvent, +0x50 buddy points, +0x64 buddy distance.
    using CtorFn11 = void*(__thiscall*)(void* self, void* player, void* other, int supportFlag, float points,
                                        const void* sidRange, int bucket, int visibility, const void* buddySid,
                                        float buddyPoints, float buddyDistance, int requestVO);
    using DeletingDtorFn = void*(__thiscall*)(void* self, int flags);
    void* context = ServerGameContext();
    if (!context || !player || !sid)
        return false;
    const char* range[2] = {sid, sid + std::strlen(sid)};
    // The buddy text is copied from an eastl::string {begin, end, capacity, allocator}; allocator 0 = the default,
    // as the game's own temporaries (0x4090E0) start out.
    const char* buddyText = buddySid ? buddySid : "";
    size_t buddyLength = std::strlen(buddyText);
    const void* buddyString[4] = {buddyText, buddyText + buddyLength, buddyText + buddyLength + 1, nullptr};
    alignas(16) uint8_t message[0x80] = {};  // the message is 0x6C bytes
    GUARDED("score event message", {
        reinterpret_cast<CtorFn11>(addr::ScoreEventMessage_ctor)(message, player, nullptr, 1, points, range, 0,
                                                                  visibility, buddyString, buddyPoints,
                                                                  buddyDistance, 0);
        *reinterpret_cast<int*>(message + 0x40) = statEvent;
        reinterpret_cast<DispatchFn>(addr::GameContext_dispatchMessage)(context, message);
        reinterpret_cast<DeletingDtorFn>(addr::ScoreEventMessage_dtor)(message, 0);
    })
}

bool GetFireteamBuddy(void* player, void** outBuddy)
{
    using GetBuddyFn = void*(__cdecl*)(void* player);
    *outBuddy = nullptr;
    void** out = outBuddy;
    GUARDED("getFireteamBuddy", *out = reinterpret_cast<GetBuddyFn>(addr::Player_getFireteamBuddy)(player))
}

bool GetSquad(void* player, int* outSquad)
{
    int extentOffset = 0;
    uint32_t squad = 0;
    if (!player || !ReadU32(reinterpret_cast<const void*>(kPlayerExtentOffset), reinterpret_cast<uint32_t*>(&extentOffset)) ||
        !ReadU32(At(player, extentOffset + kExtent_Squad), &squad))
        return false;
    *outSquad = static_cast<int>(squad);
    return true;
}

bool IsSquadLeader(void* player, bool* outLeader)
{
    int extentOffset = 0;
    uint8_t leader = 0;
    if (!player || !ReadU32(reinterpret_cast<const void*>(kPlayerExtentOffset), reinterpret_cast<uint32_t*>(&extentOffset)) ||
        !ReadByte(At(player, extentOffset + kExtent_SquadLeader), &leader))
        return false;
    *outLeader = leader != 0;
    return true;
}

bool SetSquad(void* player, int squad)
{
    using SetSquadFn = void(__thiscall*)(void* extent, int squad, int team);
    GUARDED("VeniceServerPlayerExtent::setSquad", {
        int extentOffset = *reinterpret_cast<const int*>(kPlayerExtentOffset);
        reinterpret_cast<SetSquadFn>(addr::PlayerExtent_setSquad)(static_cast<char*>(player) + extentOffset, squad, 0);
    })
}

bool GetSpottingTarget(void* soldier, void** outComponent)
{
    using GetComponentFn = void*(__thiscall*)(void* entity, int unused);
    *outComponent = nullptr;
    void** out = outComponent;
    GUARDED("getComponent<MOHWServerSpottingTargetComponent>",
            *out = reinterpret_cast<GetComponentFn>(addr::Entity_getSpottingTarget)(soldier, 0))
}

bool GetPlayerId(void* player, int* outId)
{
    using GetIdFn = int(__thiscall*)(void* player);
    void** vtable = nullptr;
    void* fn = nullptr;
    if (!player || !ProbePtr(player, reinterpret_cast<void**>(&vtable)) || !vtable || !ProbePtr(vtable + 0x1C / 4, &fn) ||
        !LooksLikeCode(reinterpret_cast<uintptr_t>(fn)))
        return false;
    int* out = outId;
    GUARDED("player getId", *out = reinterpret_cast<GetIdFn>(fn)(player))
}

bool AddScoreRecord(void* scoring, void* player)
{
    using AddFn = void(__thiscall*)(void* scoring, void* player);
    GUARDED("scoring system: add player record", reinterpret_cast<AddFn>(addr::Scoring_addPlayerRecord)(scoring, player))
}

bool RemoveScoreRecord(void* scoring, void* player)
{
    using RemoveFn = void(__thiscall*)(void* scoring, void* player);
    GUARDED("scoring system: remove player record",
            reinterpret_cast<RemoveFn>(addr::Scoring_removePlayerRecord)(scoring, player))
}

bool AddScoreKill(void* playerScore)
{
    using AddKillFn = void(__thiscall*)(void* score);
    GUARDED("PlayerScore: add kill", reinterpret_cast<AddKillFn>(addr::PlayerScore_addKill)(playerScore))
}

bool SetScorePoints(void* playerScore, int points)
{
    GUARDED("PlayerScore: set points", reinterpret_cast<SetIntFn>(addr::PlayerScore_setPoints)(playerScore, points))
}

bool ClearScoreTotals(void* playerScore)
{
    using ClearFn = void(__thiscall*)(void* score);
    GUARDED("PlayerScore: clear totals", reinterpret_cast<ClearFn>(addr::PlayerScore_clearTotals)(playerScore))
}

bool AddScoreToTotals(void* playerScore)
{
    using AddFn = void(__thiscall*)(void* score);
    GUARDED("PlayerScore: add to totals", reinterpret_cast<AddFn>(addr::PlayerScore_addToTotals)(playerScore))
}

bool GetSoldierHealth(void* soldier, float* outHealth)
{
    using HealthFn = float(__thiscall*)(void* soldier);
    void** vtable = nullptr;
    void* fn = nullptr;
    if (!soldier || !ProbePtr(soldier, reinterpret_cast<void**>(&vtable)) || !vtable ||
        !ProbePtr(vtable + 0xD8 / 4, &fn) || !LooksLikeCode(reinterpret_cast<uintptr_t>(fn)))
        return false;
    float* out = outHealth;
    GUARDED("soldier health", *out = reinterpret_cast<HealthFn>(fn)(soldier))
}

bool KillSoldier(void* soldier)
{
    using KillFn = void(__thiscall*)(void* soldier, bool viaEntry, bool suicide);
    void** vtable = nullptr;
    void* fn = nullptr;
    // Only a ServerSoldierEntity's own kill (vtable +0x17C, signature-checked): anything else is left alone.
    if (!soldier || !ProbePtr(soldier, reinterpret_cast<void**>(&vtable)) || !vtable ||
        !ProbePtr(vtable + 0x17C / 4, &fn) || reinterpret_cast<uintptr_t>(fn) != addr::ServerSoldier_kill)
        return false;
    GUARDED("ServerSoldierEntity::kill", reinterpret_cast<KillFn>(fn)(soldier, false, true))
}

bool Localize(const char* sid, char* out, size_t size)
{
    using LocalizeFn = void*(__cdecl*)(void* out, const char* sid);
    using ReleaseFn = void(__thiscall*)(void* string);
    alignas(16) uint8_t text[32] = {};  // eastl wide string {begin, end, capacity, allocator}
    out[0] = '\0';
    GUARDED("localize", reinterpret_cast<LocalizeFn>(addr::Localize)(text, sid);
            if (const wchar_t* begin = *reinterpret_cast<const wchar_t* const*>(text))
                WideCharToMultiByte(CP_UTF8, 0, begin, -1, out, static_cast<int>(size), nullptr, nullptr);
            reinterpret_cast<ReleaseFn>(addr::WideString_release)(text))
}

void* ClientPlayerManager()
{
    void* context = nullptr;
    void* manager = nullptr;
    if (!ProbePtr(reinterpret_cast<const void*>(kClientGameContextPtr), &context) || !context ||
        !ProbePtr(At(context, 0x34), &manager))
        return nullptr;
    return manager;
}

bool ClientLocalPlayer(void** outPlayer)
{
    using GetLocalFn = void*(__cdecl*)();
    *outPlayer = nullptr;
    if (!ClientPlayerManager())
        return false;
    void** out = outPlayer;
    GUARDED("client local player", *out = reinterpret_cast<GetLocalFn>(addr::Client_getLocalPlayer)())
}

bool ClientPlayerById(int id, void** outPlayer)
{
    using GetPlayerFn = void*(__thiscall*)(void* manager, int id);
    *outPlayer = nullptr;
    void* manager = ClientPlayerManager();
    if (!manager)
        return false;
    void** out = outPlayer;
    GUARDED("ClientPlayerManager::getPlayer", *out = reinterpret_cast<GetPlayerFn>(addr::ClientPlayerManager_getPlayer)(manager, id))
}

bool ClientSoldierOf(void* clientPlayer, void** outSoldier)
{
    using GetSoldierFn = void*(__thiscall*)(void* player);
    *outSoldier = nullptr;
    void** out = outSoldier;
    GUARDED("ClientPlayer::getSoldier", *out = reinterpret_cast<GetSoldierFn>(addr::ClientPlayer_getSoldier)(clientPlayer))
}

bool ClientSpottingTarget(void* clientSoldier, void** outComponent)
{
    using GetComponentFn = void*(__thiscall*)(void* entity, int unused);
    *outComponent = nullptr;
    void** out = outComponent;
    GUARDED("getComponent<MOHWClientSpottingTargetComponent>",
            *out = reinterpret_cast<GetComponentFn>(addr::ClientEntity_getSpottingTarget)(clientSoldier, 0))
}

bool ClientAddSpot(void* component, void* spotterClientPlayer, int spottedType)
{
    using AddSpotFn = void(__thiscall*)(void* component, void* spotter, int type);
    GUARDED("MOHWClientSpottingTargetComponent::addSpot",
            reinterpret_cast<AddSpotFn>(addr::ClientSpottingTarget_addSpot)(component, spotterClientPlayer, spottedType))
}

bool AddSpot(void* component, void* spotterPlayer, int spottedType)
{
    using AddSpotFn = void(__thiscall*)(void* component, void* spotter, int type);
    GUARDED("MOHWServerSpottingTargetComponent::addSpot",
            reinterpret_cast<AddSpotFn>(addr::SpottingTarget_addSpot)(component, spotterPlayer, spottedType))
}

}  // namespace eng
