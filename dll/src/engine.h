#pragma once
// Bindings to lanmohw.exe (MOHW, 32-bit, image base 0x400000). Addresses and offsets come from re/RE_NOTES.md;
// every function address is checked against signatures.inc before anything is called.
#include <cstddef>
#include <cstdint>

namespace eng {

// --- Globals ---------------------------------------------------------------------------------------------------
constexpr uintptr_t kServerGameContextPtr = 0x2AEAF8C;  // ServerGameContext*
constexpr uintptr_t kClientGameContextPtr = 0x2AC5FE4;  // ClientGameContext* (+0x34 ClientPlayerManager)
constexpr uintptr_t kPlayerExtentOffset = 0x2A1B784;    // int: VeniceServerPlayerExtent offset inside ServerPlayer

constexpr uintptr_t kSettingsManagerPtr = 0x2A5E10C;      // SettingsManager*
constexpr uintptr_t kTypeInfo_ClientSettings = 0x2B7C34C;
constexpr uintptr_t kTypeInfo_ServerSettings = 0x2B779BC;
constexpr uintptr_t kTypeInfo_NetworkSettings = 0x2B76234;
constexpr uintptr_t kTypeInfo_GameSettings = 0x2B74450;
// The server's game preset, synced to clients (NetworkSettingsMessage, 0xE40880 -> 0xE2D6A0); the HUD reads these
// flags by name through 0xF54A20 (NoMinimap, NoHud, NoMinimapSpotting, No3dSpotting, NoNameTag).
constexpr uintptr_t kTypeInfo_SyncedGameSettings = 0x2B7447C;  // +0x1C DisableRegenerateHealth, +0x1D EnableFriendlyFire
constexpr uintptr_t kTypeInfo_SyncedBFSettings = 0x2B949B4;    // +0x10 NoMinimap, +0x12 NoMinimapSpotting, +0x13 No3dSpotting
// BFServerSettings: +0x1C NoInteractivityTimeoutTime (idle kick, read by 0xE1E000; under 0.001 never kicks), +0x20
// NoInteractivityThresholdLimit, +0x28 TeamKillCountForKick, +0x2C TeamKillValueForKick (0 skips each, 0xE726F0).
constexpr uintptr_t kTypeInfo_BFServerSettings = 0x2B94988;
constexpr int kBFServerSettings_IdleTimeout = 0x1C;
constexpr int kBFServerSettings_TeamKillCountForKick = 0x28;
constexpr int kBFServerSettings_TeamKillValueForKick = 0x2C;
constexpr uintptr_t kTypeInfo_SyncedMOHWSettings = 0x2B949E0;  // +0x0C HudNoMinimap
constexpr uintptr_t kServerPhysicsPtr = 0x2AEAF88;  // g_ServerPhysics (created at server startup, 0xA9DAF0)
// rayQuery flags (VU RayCastFlags; spawn visibility uses 0x34 = no water, no ragdoll, no characters).
constexpr uint32_t kRay_DontCheckWater = 0x04;
constexpr uint32_t kRay_DontCheckRagdoll = 0x10;
constexpr uint32_t kRay_DontCheckCharacter = 0x20;
constexpr int kKit_MPKitType = 0x3C;  // VeniceSoldierCustomizationAsset: 4 = Sniper, 6 = Support, ...
// Unlock classes. ServerPlayer+0x524 may only hold UnlockAsset (and subclasses): spawn customisation reads +0x24 of
// every entry as UnlockAsset.LinkedTo. SoldierWeaponUnlockAsset (a sibling, +0x24 = Weapon blueprint) goes to the
// separate weapon set instead; putting one in +0x524 made the spawn call through garbage and freeze the game.
constexpr uintptr_t kTypeInfo_UnlockAsset = 0x2B8119C;
constexpr uintptr_t kTypeInfo_SoldierWeaponUnlockAsset = 0x2B81204;
// ClassInfo: +0x14 class id, +0x16 id of its last subclass (filled at startup); isKindOf = id in [first, last].
constexpr int kTypeInfo_ClassId = 0x14;
constexpr int kTypeInfo_LastSubclassId = 0x16;

// --- Field offsets -------------------------------------------------------------------------------------------
// Connection timeouts (defaults 5 s; client copies them into each connection when it is created, 0x878CD0).
constexpr int kClientSettings_LoadedTimeout = 0x1C;
constexpr int kClientSettings_LoadingTimeout = 0x24;
constexpr int kClientSettings_IngameTimeout = 0x8C;
constexpr int kServerSettings_IngameTimeout = 0x58;
constexpr int kServerSettings_LoadingTimeout = 0x84;
// Transport-level "no packets received" timeout; client (0x887DD0) and server (0xB0B280) peers pass it to 0x618A80,
// which stores max(timeout, rtt-based minimum) in every connection.
constexpr int kNetworkSettings_ConnectTimeout = 0x40;
// "Criterion=Value;..." (CString), parsed by 0x7E7D70 into the LevelSetup of ClientStartMultiplayerMessage.
constexpr int kGameSettings_DefaultLayerInclusion = 0x14;
// SubWorldInclusion*: +0x0C Array<SubWorldInclusionCriterion*>; criterion +0x08 Name, +0x0C Array<CString> Options.
constexpr int kGameSettings_LayerInclusionTable = 0x60;
constexpr int kCtx_Level = 0x10;
constexpr int kCtx_PlayerManager = 0x30;
constexpr int kLevel_Teams = 0x64;
constexpr int kLevel_Name = 0x68;  // char* "MOHW/Levels/MP/MP_03/MP_03" (server level)
constexpr int kTeams_Count = 0x44;  // teams[i] = TeamEntityData* for i < count
constexpr int kMgr_MaxPlayers = 0x08;
constexpr int kMgr_PlayersBegin = 0x78;
constexpr int kMgr_PlayersEnd = 0x7C;
constexpr int kPlayer_IsAI = 0x354;
constexpr int kClientSoldier_Player = 0xC4;
constexpr int kPlayer_Team = 0x38C;
constexpr int kPlayer_Input = 0x428;
constexpr int kPlayer_Soldier = 0x42C;
constexpr int kPlayer_Controlled = 0x450;
constexpr int kPlayer_SoldierAsset = 0x4E0;
constexpr int kEntity_Data = 0x14;  // Entity -> its EntityData (DataContainer, refcount at +4)
constexpr int kTeamEntityData_Team = 0x70;
constexpr int kTeamData_Soldier = 0x0C;
constexpr int kTeamData_CustomizedSoldier = 0x10;
constexpr int kTeamData_SoldierCustomization = 0x1C;  // FB2 array: element pointer, count at [-4]
// ServerPlayer+0x524: eastl::vector<UnlockAssetBase*> {begin, end, capacity} of selected unlocks (body, head, gear,
// weapons). Filled from the client's NetworkPlayerSelectedUnlockAssetsMessage; spawn customisation (0xB40120) builds
// the soldier from it, so a bot with an empty list spawns with no body: invisible and impossible to hit.
constexpr int kPlayer_Unlocks = 0x524;
// SoldierCustomizationAsset (a kit): +0x18 VisualTable, +0x1C SpecializationTable, +0x24 WeaponTable
// (CustomizationTable*). Kit_Sniper: VisualTable = the kit unlock (automatic), SpecializationTable = a soldier
// appearance (automatic, soldiers/visual/mp/backup/co_assault_backup), WeaponTable = 10 parts of weapons/gadgets.
constexpr int kKit_VisualTable = 0x18;
constexpr int kKit_SpecializationTable = 0x1C;
constexpr int kKit_WeaponTable = 0x24;
constexpr int kCustomizationTable_UnlockParts = 0x08;  // Array<CustomizationUnlockParts*>
constexpr int kUnlockParts_Selectable = 0x0C;          // Array<UnlockAssetBase*> SelectableUnlocks
constexpr int kUnlockParts_Automatic = 0x10;           // Array<UnlockAssetBase*> AutomaticUnlocks
constexpr int kAsset_Name = 0x08;                      // CString
// Online stats object (null offline); the guards in guards.cpp skip the code that uses it without a check.
constexpr uintptr_t kOnlineStatsPtr = 0x2B14928;
// ControllableEntityData (a soldier's data, entity+0x14): seconds after combat before others may spawn on it.
constexpr int kControllableData_CantSpawnOnBuddyCombatTime = 0x90;
// VeniceServerPlayerExtent (player + *kPlayerExtentOffset): +0xF4 squad (fireteam) id, 5 = none.
constexpr int kExtent_Squad = 0xF4;
constexpr int kNoSquad = 5;
constexpr int kExtent_SquadLeader = 0xF8;  // byte (setSquad, 0xE1C850)
// SpottedType (the MOHW spotting target's lists): 0 active, 1 passive, 2 victim, 3 MQ9 (UAV), 4 flash.
constexpr int kSpottedPassive = 1;
constexpr int kSpottedMQ9 = 3;
constexpr int kServer_UpdateLock = 0xB8;              // CRITICAL_SECTION inside Server
// A soldier's CharacterPhysicsEntity (soldier+0x1A4, stepped by 0x49C880): +0x80 its pose, +0x84 the pose it is
// changing to (setPose 0x49A3E0; getters 0x48B6E0 / 0x48B6F0), +0x64 -> its current character state, whose +0x10 is
// the CharacterStateType (getter 0x1A15370).
constexpr int kSoldier_CharacterPhysics = 0x1A4;
constexpr int kCharPhysics_Pose = 0x80;
constexpr int kCharPhysics_TargetPose = 0x84;
constexpr int kCharPhysics_State = 0x64;
constexpr int kCharState_Type = 0x10;
enum Pose : int  // CharacterPoseType
{
    kPoseStand = 0,
    kPoseCrouch = 1,
    kPoseProne = 2,
};
enum CharacterState : int  // CharacterStateType (MOHW's user states 5-10)
{
    kStateOnGround = 0,
    kStateJumping = 1,
    kStateInAir = 2,
    kStateClimbing = 3,
    kStateFalling = 4,
    kStateParachute = 5,
    kStateSwimming = 6,
    kStateAnimationControlled = 7,  // vaults and other special moves
    kStateSliding = 8,              // down a slope too steep to stand on
    kStateSprintSliding = 9,
    kStateRappeling = 10,
};

// EntryInput: engine size is ~0x530; allocate extra headroom.
constexpr size_t kEntryInputAlloc = 0x600;
constexpr int kInput_Analog = 0x000;  // float[12]: EIA 0..11
constexpr int kInput_DigitalBits = 0x3B8;  // u64: bit (action - 12)
constexpr int kInput_Flags = 0x3C0;   // bit0 = AuthoritativeAiming
constexpr int kInput_DeltaTime = 0x3C4;  // singleStepEntry steps the character by this (x time scale)
constexpr int kInput_Yaw = 0x3CC;
constexpr int kInput_Pitch = 0x3D0;

// EntryInputActionEnum values used so far (MOHW, from the SDK dump).
enum Action : int
{
    EIAThrottle = 0,
    EIAStrafe = 1,
    EIABrake = 2,
    EIAYaw = 4,
    EIAPitch = 5,
    EIAFire = 9,
    EIACrouch = 17,
    EIAReload = 18,
    EIAThrowGrenade = 29,
    EIAJump = 14,
    EIAZoom = 13,  // aim down sights (held: the input's toggle flag, +0x3C0 bit 0x10, is off - 0x82B9B0)
    EIASprint = 30,
};

// --- Setup ---------------------------------------------------------------------------------------------------
bool VerifySignatures();  // logs each mismatch; false means: do not touch the engine
uintptr_t Address(const char* signatureName);

// --- Safe memory access (SEH guarded) ------------------------------------------------------------------------
bool ReadU32(const void* address, uint32_t* out);
bool ReadPtr(const void* address, void** out);
bool ReadByte(const void* address, uint8_t* out);
bool ReadBytes(const void* address, void* out, size_t count);
bool ReadFloat(const void* address, float* out);
bool WriteBytes(void* address, const void* data, size_t count);

// Same as the reads above but silent on a fault: for diagnostic scans that expect to hit non-pointers.
bool ProbePtr(const void* address, void** out);
bool ProbeByte(const void* address, uint8_t* out);
// True for an address in lanmohw.exe code (.text/ctr).
bool LooksLikeCode(uintptr_t address);
// True for an address in lanmohw.exe code right after a call instruction: a plausible return address.
bool LooksLikeReturnAddress(uintptr_t address);

inline const void* At(const void* base, int offset) { return static_cast<const char*>(base) + offset; }

// --- Engine calls (SEH guarded; false + log on a fault) ------------------------------------------------------
void* ServerGameContext();
bool GetSettingsContainer(uintptr_t typeInfo, void** outContainer);  // SettingsManager::getContainer; null if not yet
bool CreatePlayer(void* playerManager, void** outPlayer);
bool SetPlayerName(void* playerManager, void* player, const char* name);
bool SetIsAIPlayer(void* player, bool isAI);
bool SetTeam(void* player, int team);
bool SetSelectedSoldierAsset(void* player, void* asset);
bool SetInput(void* player, void* input);
bool GetPlayerLoadoutState(void* player, void** outState);
bool SetKitIndex(void* loadoutState, int index);
bool AddPlayerUnlock(void* player, void* unlock);  // push_back onto ServerPlayer+0x524 via the engine's insert
// The game's own class test (e.g. 0xB401C8): object->getType() (vtable slot 0), then class-id range of typeInfo.
bool IsKindOf(void* object, uintptr_t typeInfo);
// Runs the server's "player selected unlocks" handler (0xB41B70) with a message holding only `kit`: the game's own
// default-loadout path (kit, first weapon of every weapon slot with default attachments, default appearance).
bool ApplyDefaultLoadout(void* player, void* kit);
// In MP, soldiers are single-stepped: the server runs a character's input + physics (CharacterPhysicsEntity::step)
// only when that player's input frame is processed - per network packet for humans, by the AI system for AI. MP
// levels have no AI system, so a bot soldier is never stepped (no ground snap, no hittable physics) unless we do it.
// `force` steps regardless of shouldSingleStep(), as fun-bots does every tick in VU ("engine-requirement"): bot
// soldiers report "not single-step" yet nothing else steps them - they turned but never walked.
// `stepped` = whether singleStepEntry ran. Returns false on fault.
bool StepSoldier(void* soldier, bool force, bool* stepped);
// ServerPhysics::rayQuery from -> to (world positions); *blocked = something in between. Server thread only.
bool RayBlocked(const float from[3], const float to[3], uint32_t flags, bool* blocked);
// As RayBlocked, plus where it hit: *fraction = 0..1 along from -> to (RayCastHit +0x30; +0x00 is the position, +0x10
// the normal), 1 when nothing was hit.
bool RayHit(const float from[3], const float to[3], uint32_t flags, bool* blocked, float* fraction);
uint32_t TakeRayCount();  // rays cast since the last call (performance log)
// The soldier's pose (stand / crouch / prone, and the one it is changing to) and character state. Any may be null.
bool GetCharacterPose(void* soldier, int* pose, int* targetPose, int* state);
// How deep the water is at this spot (0 = none): a ray from 2 m above `feet` to 8 m below it, once counting water
// and once not; the gap between where they hit is the depth (a spot over a drop into water counts too: nothing solid
// before the water). Server thread only.
float WaterDepth(const float feet[3]);
inline bool WaterAt(const float feet[3], float depth) { return WaterDepth(feet) > depth; }
const char* PoseName(int pose);
const char* StateName(int state);
bool ConstructEntryInput(void* buffer);
bool DestroyEntryInput(void* input);
bool SetAuthoritativeAiming(void* input, bool on);
bool ForceRespawn(void* serverGameContext, void* player);  // dispatches ServerPlayerForceRespawnMessage
// Deletes an AI player that has no soldier, as the game does when an AI soldier dies (0xB61C11): its loadout released
// (0xB42D40), then ServerPlayerManager::deletePlayer (0xB3AC90), which frees it. Server thread; clear its input first.
bool DeleteAiPlayer(void* serverGameContext, void* player);
// In-place level change (levelchange.cpp), as the server does for a client's NetworkRequestLoadLevelMessage (0xAA8900):
// a ServerLoadLevelMessage with a LevelSetup (levelPath "MOHW/Levels/MP/MP_03/MP_03", GameMode option, the synced difficulty,
// ForceReloadResources so the same level loads again) queued 0.6 s ahead, then ServerRoundResetMessage. Server thread. Null
// when queued, else what went wrong.
const char* RequestLevelLoad(const char* levelPath, const char* gameMode, bool forceReload);
// The server's level-load state (its ServerLoadLevelMessage handler 0xAAB140): bytes +0x40 loading, +0x41 unload pending,
// +0x42, +0x6C same level, +0x6D load asked for, +0x6E; and +0x7C the level it has.
bool ReadServerLoadFlags(void* server, uint8_t flags[6], void** currentLevel);
bool SpawnAt(void* player, const float* transform16);     // VeniceServerPlayerExtent::spawnAt, 16-byte aligned
bool GetSoldierTransform(void* soldier, const float** outTransform16);  // right, up, forward, trans (vec4 each)
// The game's score message (type 0x6182A896, built by 0xE1DBF0), as the online stats object sends it for each scoring
// event: the scoring system adds `points` to the player's round score and shows "+points <sid>" on their HUD, gives
// their fireteam buddy `buddyPoints` within `buddyDistance` m, and the support-action collection adds the points to
// the special-ability meter (and buddyPoints to the buddy's). statEvent = StatEvent id (cosmetic here), visibility =
// ScoringTypeData.VisibilityType, buddySid = the buddy's popup text.
bool SendScoreEvent(void* player, int statEvent, float points, const char* sid, int visibility, float buddyPoints,
                    const char* buddySid, float buddyDistance);
// The game's fireteam pairing (0xE3A8A0): the teammate whose player extent reports the same fireteam; null if none.
bool GetFireteamBuddy(void* player, void** outBuddy);
bool GetSquad(void* player, int* outSquad);  // extent +0xF4
bool IsSquadLeader(void* player, bool* outLeader);
bool SetSquad(void* player, int squad);      // the game's own setSquad (0xE1C850): leader, message, replication
// A soldier's MOHWServerSpottingTargetComponent (0xE02780); null if it has none.
bool GetSpottingTarget(void* soldier, void** outComponent);
// Spots that soldier for `spotter`'s team: the game's addSpot (0x169C220), as a client spot report or an explosion's
// SpotTypeOnExplode does; the spot lasts the type's time from the component data and refreshes per spotter.
bool AddSpot(void* component, void* spotterPlayer, int spottedType);
bool GetPlayerId(void* player, int* outId);  // vfunc +0x1C, server or client player (the id replication uses)
// The scoring system's per-player records ([scoring +8] vector by player id; record +0xA8 = the replicated PlayerScore:
// +0x140 kills, +0x144 deaths, +0x148 points). `scoring` = its ServerPlayer listener's +4.
bool AddScoreRecord(void* scoring, void* player);     // 0xE7C710: record + PlayerScore, as for a joining human
bool RemoveScoreRecord(void* scoring, void* player);  // 0xE7A650: frees both, clears the slot
bool AddScoreKill(void* playerScore);                 // 0xE15A80: kills + 1 (replicated)
bool SetScorePoints(void* playerScore, int points);   // 0xE15C00: points (replicated)
// The match totals the end-of-round screens show (+0x168 kills, +0x16C deaths, +0x170 points, +0x174 flags; dirty
// 0x400): the game adds the round's values to them on ServerRoundSubRoundOverMessage (0xE66F20).
bool ClearScoreTotals(void* playerScore);             // 0xE15DB0: totals = 0
bool AddScoreToTotals(void* playerScore);             // 0xE15D70: totals += kills, deaths, points, flags
// A server soldier's health: its own vfunc +0xD8 (0xAD4070), which the damage handler reads (0xE35EE0, before any
// armour bonus) when it builds ServerSoldierSoldierDamageMessage.
bool GetSoldierHealth(void* soldier, float* outHealth);
// Kills a server soldier the way the game kills a player who changes class while alive: the server's
// NetworkSuicideMessage handler (0xB42590) calls ServerSoldierEntity::kill(0, 1) (vtable +0x17C, 0xB5E790) - a hit of
// its whole health through its damage handler, marked a suicide. False (nothing done) if it isn't one. Server thread.
bool KillSoldier(void* soldier);
// One step of a ServerCapturePointEntity's capture (0xE47F30; gen_signatures.py CapturePoint_update): `inside` is
// {ServerPlayer** begin, end} (scoring awards them capture points), `counts` int[16] (attackers, holders, then squads).
// Server thread.
bool CapturePointUpdate(void* capturePoint, float dt, void* inside, void* counts, int attacker);
// A capture point's nation for a team (0xE36AA0: +0xDC team 1, +0xE0 team 2; an index into its FlagTemplates).
bool CapturePointSetTeamNation(void* capturePoint, int team, int nation);
// A UI data value set to a string (0xA787B0, thiscall(const char*): type 6). UI thread.
bool UiSetString(void* value, const char* text);

// --- Client side: main thread only (the XInput poll runs there) ----------------------------------------------
// The game's text for a string id ("ID_M_LEVEL_NAME_MP_05" -> the map's name), as UTF-8; the id itself if it has none.
bool Localize(const char* sid, char* out, size_t size);
// MOHWClientSpottingTargetComponent: +0x18 unspottable, lists {begin, end} of 8-byte entries at +0x20 active,
// +0x30 passive, +0x40 victim, +0x50 MQ9, +0x60 flash, +0x70 local; the minimap (0x1786230 -> 0x17850A0) draws an enemy
// whose active or MQ9 list (or victim list with you as spotter) has entries.
constexpr int kClientSpot_Unspottable = 0x18;
constexpr int kClientSpot_Lists = 0x20;
void* ClientPlayerManager();
bool ClientLocalPlayer(void** outPlayer);
bool ClientPlayerById(int id, void** outPlayer);
bool ClientSoldierOf(void* clientPlayer, void** outSoldier);
bool ClientSpottingTarget(void* clientSoldier, void** outComponent);
// The client's own addSpot (0x16B5D90), what your game does when you spot someone: into list `spottedType` for the
// type's time, refreshed per spotter.
bool ClientAddSpot(void* component, void* spotterClientPlayer, int spottedType);

using ServerUpdateFn = bool(__thiscall*)(void* server, void* updateInfo);

}  // namespace eng
