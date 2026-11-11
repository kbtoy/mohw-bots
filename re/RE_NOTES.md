# MOHW reverse-engineering notes

Target: `re/lanpack/lanmohw.exe` — MOHW, PE32, image base `0x400000`, no relocations (addresses below are absolute VAs).

## TypeInfo SDK (done, static)

`tools/fb2_typeinfo_dump.py re/lanpack/lanmohw.exe re/sdk` → `re/sdk/types.json`, `re/sdk/sdk.h`, `re/sdk/summary.txt`.

- **8,685 types**: 3,323 classes, 1,426 value types, 558 enums, 3,351 array types, 22 primitives. All field types resolve.
- How it works: const `*InfoData` blocks live in the `typeinfo`/`fieldinf` sections; the runtime `TypeInfo`
  objects are zero on disk and are built by static constructors in the `ctr` section:
  `push <data>; mov ecx, <TypeInfo*>; call <ctor>`. Some array TypeInfos are embedded at `+4` in a
  vtable-prefixed object (ctor `0x55f2a0`), so fields reference `obj+4`.
- Layouts match txt231/FBTools `Sdk/Fb2` exactly (see dumper docstring).
- Reflected **data** classes (`*Data`, assets, settings) have full fields + offsets. Reflected **runtime**
  classes (`ServerSoldierEntity` etc.) have name, size, hierarchy, TypeInfo address — no fields.
- No runtime run needed. Re-running takes ~1 s.

## Facts the bot port needs

### Input actions — MOHW values differ from BF3
fun-bots' actions mapped to MOHW `EntryInputActionEnum`:

| fun-bots (BF3) | MOHW value | | fun-bots (BF3) | MOHW value |
|---|---|---|---|---|
| EIAThrottle | 0 | | EIAJump | 14 |
| EIAStrafe | 1 | | EIAReload | 18 |
| EIABrake | 2 | | EIASelectWeapon1–4 | 19–22 |
| EIAYaw | 4 | | EIAMeleeAttack | 28 |
| EIAPitch | 5 | | EIASprint | 30 |
| EIARoll | 6 | | EIAInteract | 33 |
| EIAFire | 9 | | EIAToggleParachute | 34 |
| EIACameraPitch | 10 | | EIACycleRadioChannel | 35 |
| EIACameraYaw | 11 | | EIAZoom | 13 |
| EIAFireCountermeasure | 12 | | | |

**Not in MOHW:** `EIAQuicktimeJumpClimb`, `EIAQuicktimeFastMelee`, `EIASelectWeapon5–7`.
MOHW instead has `EIAThrowGrenade 29`, `EIAStaticGadget 25`, `EIADynamicGadget1/2 26/27`,
`EIAGrenadeLauncher 24`, `EIAClassAbility 57`, `EIASupportAction_Offensive/Defensive 48/49`,
`EIAPeek 7`, `EIALean 8`, `EIAPeekAndLean 42`, `EIASprintSlide 50`. Full list in `sdk.h`.

### Settings
- `SyncedGameSettings.AllowClientSideDamageArbitration` — bool at `+0x1e` (bot bullets need this off, as in fun-bots).
- `ServerSettings`: `IsAiEnabled +0x99`, `AILooksIntoCamera +0x98`, `PlayerCountNeededForMultiplayer +0x70`,
  `InputPolicy +0x30`, `RespawnTimeModifier +0x68`, `ServerName +0x38`, `AdministrationPassword +0x20`.
- No `DisablePreRound` anywhere. Pre-round is `ServerPreRoundEntity` (size `0xbc`).
- `CharacterPoseType`: Stand 0, Crouch 1, Prone 2 (same as BF3).

### Runtime classes (size / parent / TypeInfo)
| Class | Size | Parent | TypeInfo |
|---|---|---|---|
| ServerSoldierEntity | 0x7f0 | ServerCharacterEntity | 0x2b6e0d4 |
| ServerCharacterEntity | 0x130 | ServerControllableEntity | 0x2b6afe8 |
| ServerControllableEntity | 0xf8 | ServerPhysicsEntity | 0x2b6dab4 |
| ServerPlayerEntryComponent | 0xc8 | ServerEntryComponent | 0x2b6d99c |
| ServerSoldierWeapon | 0x40c | ServerGameEntity | 0x2b6f0bc |
| ServerBulletEntity | 0x170 | ServerProjectileEntity | 0x2b6f240 |
| ServerCharacterSpawnEntity | 0x16a0 | ServerSpawnEntity | 0x2b6bcb0 |
| CharacterPhysicsEntity | 0x260 | PhysicsEntityBase | 0x2b533ac |

`ServerPlayerManager` / `ServerGameContext` / `EntryInputState` aren't reflected — find them through code.

## MOHW's native AI-player pipeline (traced 2026-10-22, static)

Not in BF3/VU. MOHW's campaign spawns AI soldiers as real `ServerPlayer`s with no network connection,
then attaches an AI brain via `AISpawnBotMessage`. Function names below are mine; addresses are in `re/labels.json`.

### Core singletons
- **`g_ServerGameContext` = `*(ServerGameContext**)0x2aeaf8c`** (1,405 references). 0x48-byte object from the server
  arena `0x2a59130`; created by `0xa98e10`, destroyed by `0xa9f9b0`.
  - `+0x30` = **`ServerPlayerManager*`** (setter `0xa98e60` writes it to both `+0x08` and `+0x30`, as in BF3).
  - `GameContext::dispatchMessage(msg)` = `0x5928c0` (`ecx = g_ServerGameContext`).

### Player creation
- **`ServerPlayerManager::createPlayer(connection, id, isSpectator)` = `0xb43a10`** (thiscall, `ret 0xc`).
  Pass `connection = nullptr, id = -1` (auto-allocate), `isSpectator = false`. Checks max players (`mgr+0x08`)
  and max spectators (`mgr+0x98`), allocates an id from the bitset at `mgr+0xb0`, builds the player via factory `0xb42f60`,
  runs player-created listeners, stores it at `mgr+0xa4[id]`. Players vector `mgr+0x78/+0x7c`, spectators `mgr+0x88/+0x8c`.
- Name: `ServerPlayerManager::0xb3eb20(player, name)`.
- `ServerPlayer` fields (from setters):
  | Offset | Meaning | Setter |
  |---|---|---|
  | `+0x354` | isAIPlayer (bool) | `0xb2ada0` |
  | `+0x356` | isCheapAI (bool) | `0xb2add0` |
  | `+0x389` / `+0x38a` | force show / pin nametag | `0xb2adb0` |
  | `+0xe70` | InvisibilityToAIMode | `0xb2af90` |
  | `+0x507` | unknown bool (`0xb2ae10` returns `+0x507 \|\| +0x354`) | |
  | `+0x42c` | pointer to an object with a flag byte at `+0x7ce` | |
  | vtable `+0x1c` | `getId()` | |
- `ServerCharacterEntity + 0x10c` = owning `ServerPlayer*`.

### Spawn path — `ServerCharacterSpawnEntity::spawn(player, a2, a3, a4)` = `0xb1e580`
Driven by level data `CharacterSpawnReferenceObjectData` (`+0x150 PlayerType`, a `PlayerSpawnType`:
`HumanPlayer 0, AiPlayer 1, Actor 2, BrainAI 3, CheapAI 4`).
1. Creates the controllable (`0xba7a60`) and verifies it's a `ServerCharacterEntity` subclass.
2. If no player was given and `PlayerType` ∉ {Human, Actor}: `createPlayer(nullptr, a2, false)` on
   `g_ServerGameContext->+0x30`, then names it from an entity-bus `CString` property.
3. `setIsAIPlayer(PlayerType != Human)`, `setNametagFlags(...)`, `setIsCheapAI(PlayerType == CheapAI)`,
   `0xb3c940(controllable)`, `setInvisibilityToAIMode(data+0x180)`.
4. Checks the spawned controllable is a `ServerSoldierEntity`, applies template/behaviour data.
5. **Only for `PlayerType == AiPlayer`:** builds and dispatches `AISpawnBotMessage`.
   `0xb22ec0` re-sends it for already-spawned characters whose player has `isAI && !isCheapAI`.

### `AISpawnBotMessage` (0x40 bytes, type hash `0x94a37fc4`, ctor `0x25a0ad0`, vtable `0x27b690c`)
`fb::Message` header: `+0x00` vtable, `+0x08` type hash; ctor `0x584680(category 0x59722d, hash)`.
| Offset | Field | Source in the sender |
|---|---|---|
| `+0x28` | `ServerPlayer*` (the AI player) | |
| `+0x2c` | `SoldierSpawnTemplateData*` | `CharacterSpawnReferenceObjectData.Template` (+0x168) |
| `+0x30` | `ServerCharacterSpawnEntity*` (spawner; handler reads `+0x1684`) | `this` |
| `+0x34` | float | `HumanTargetBiasMultiplier` (+0x18c) |
| `+0x38` | bool | `HumanTargetBiasOverride` (+0x19e) |
| `+0x39` | bool | `NeverLeaveVehicle` (+0x199) |
| `+0x3a` | bool | `AllowedToThrowGrenades` (+0x19c) |
| `+0x3b` | bool | always 1 → bot added to the list at `system+0x2dc` |

### Handler — `0x1daeb20` (AI bot system `onMessage`, switch on `msg+0x08`)
For `0x94a37fc4`: validates the player, then `acquireBot(player)` (`0x1dad740`):
**fixed pool of 32 bot slots** at `system+0x04..+0x104` (8 bytes each: `Bot*`, flags), mapped by `player->getId()`
through the vector at `system+0x104`. It then applies `Template->Sensing` (+0x18) and `->Behaviour` (+0x1c), sets the brain
(`bot+0x314`): difficulty float, `+0x1bc`, `+0x1bd`, `+0x40` from the message bools, and finally dispatches
`AIBotSpawnedMessage` (hash `0x4ad98145`, `+0x28 = player id`). `releaseBot(playerId)` = `0x1dad820`.

### What this means for bots in MP
- **Player creation is solved natively:** `createPlayer(nullptr, -1, false)` + `setIsAIPlayer(true)` gives a connectionless
  player — the thing VU's `PlayerManager:CreatePlayer` wraps for fun-bots.
- Brain: see the decision below — MOHW's own AI can't navigate MP maps.

## Game data findings (install at `F:\Medal of Honor Warfighter`, 2026-10-22)

New read-only tools: `tools/fb2_sbtoc.py` (TOC/superbundle), `tools/fb2_cas.py` (CAS catalog + decompression),
`tools/fb2_ebx.py` (EBX partitions). Format notes for MOHW vs BF3 are in each docstring
(TOCs are always XOR-obfuscated; CAS zlib blocks use type `0x0000`; EBX instances are packed with no alignment padding).

**`LevelData.PathfindingBlobInfo` (navmesh) and `LevelData.AISystem`, per level:**
| Levels | Navmesh | AISystem |
|---|---|---|
| MP_03, 05, 10, 12, 13, 16, 18, 19 (all base MP) | **0 bytes** | **null** |
| All 11 SP levels | 10 KB – 2.6 MB (2–48 chunks) | `ext:5a052c75…/1ebb1fbc…` |
| MP_20, MP_21 (Xpack1 DLC) | not checked — non-CAS superbundles, not parsed yet | |

Also: no MP level loads `mohw/ai/settings/navigationconstants` (every SP level does). MP game-mode bundles do load
`mohw/ai/entries/*`, `mohw/ai/behaviours_new` and AI weapons — for the AI-driven support-action helicopters, which follow scripted paths.

### Decision: fun-bots-style brain on native AI players
MOHW's campaign AI needs a navmesh and an `AISystem` entity; MP levels ship neither, so `AISpawnBotMessage` bots
would have no way to move. The plan:
- **Use MOHW's native pipeline for the player** (`createPlayer(nullptr, …)`, `setIsAIPlayer`) — no fake connection needed.
- **Port fun-bots' brain** (waypoint traces + direct input) for movement and combat, as originally planned.
- Optional later: reuse MOHW's AI templates (difficulty/accuracy data) for tuning.

## Messages (tooling)
`tools/msghash.py` → `re/messages.json`: type hash → name for all 775 reflected `*Message` types (+ category, vtable, ctor).
`tools/msginfo.py <Name>`: a message's ctor, senders and handlers. FB2 message header: `+0x00` vtable (slot 0 = `getType`),
`+0x04` category hash, `+0x08` type hash, payload from `+0x28`. Handlers switch on `+0x04` then `+0x08`.
Categories: `0xc67d277b` Network (client→server), `0xf3c93dd3` ServerPlayer, `0x0059722d` AI.

## MP soldier spawn (traced 2026-10-23, static)

**Spawn points:** MP_03 TDM has no `CharacterSpawn` spawners — it has 49 `AlternateSpawnEntityData`
(`Transform`, `Team` 0/1/2, `Priority`, `Enabled`; 25 shared, 10 team 1, 13 team 2), chosen by a tactical spawn-point chooser.

**Request → spawn chain** (handler `0xe40020`, class unknown):
| Trigger | Handler action |
|---|---|
| `NetworkSpawnMessage` (`0x9399590b`, from a client) | player ← connection; reject if `player+0x430` set; `setSelectedSoldierAsset(msg+0x38)`; `spawnPlayer(conn, player, 1, 1)` |
| `NetworkSelectedKitMessage` (`0xb634ba6f`) | `getPlayerLoadoutState(player)->setKitIndex(msg+0x38)` (validated against the team's kit list) |
| **`ServerPlayerForceRespawnMessage`** (`0x5decda09`, payload `+0x28 = player`) | destroy old soldier, **`spawnPlayer(0, player, 1, 0)` — no connection needed** |
| `ServerPlayerSwitchTeamMessage` (`0xdac6ceb7`) | extent `0xe23430` |

**`spawnPlayer(connection, player, force, a4)` = `0xe3a940`:**
1. If `g_SpawnPointChooser` (`*0x2af3984`) exists: unless `force`, require `canPlayerSpawn(player)` (`0xb17400`);
   then `chooser->vfunc0(player, player+0x38c /*team*/, &transform)` picks a point.
2. `extent = player + *(0x2a1b784)` (game-specific `VeniceServerPlayerExtent`), then **`extent->spawnAt(player, &transform, a4, 1)` (`0xe1e9d0`)**:
   - **`createSoldier(player+0x4e0 asset, player, transform)` = `0xe1e710`** — builds an `EntityCreationInfo` at the transform and
     instantiates the asset via the entity factory (`0xace630`) in the level's realm. (VU: `player:CreateSoldier(kit, transform)`.)
   - **`spawnSoldierAt(soldier, transform, 0, a4)` = `0xe16790`** — destroys the old soldier (`player+0x42c`), places the new one,
     `ServerPlayer::0xb3c940(soldier)` (attach), applies kit/customization (`0xb40120`), activates (`0xb2e6b0`). (VU: `SpawnSoldierAt`.)
   - Sets health, dispatches `ServerPlayerSpawnOnSelectedSpawnPointMessage`.
3. Without a chooser: extent fallback `0xe23570` / `0xe23830`.

**`ServerPlayer` fields added:** `+0x38c` team, `+0x41c` loadout state (`+0x44` kit index), `+0x428` **`EntryInput*`**,
`+0x42c` **soldier**, `+0x430` another controlled entity (blocks spawning), `+0x450` controlled controllable,
`+0x4e0` selected soldier asset (refcounted). Entities: `+0x14` = data.

**Bot spawn recipe (to verify at runtime):** see "Putting it together" below.

## Input (traced 2026-10-23, static)

**`EntryInput`** (VU's `EntryInput`, MOHW's `EntryInputState`) — ~0x530 bytes, no vtable. Ctor `0x80ef00`, dtor `0x8078d0`,
copy `0x80ef50`. Static default instance `0x2af1160`.
| Offset | Field | Accessors |
|---|---|---|
| `+0x000` | `float analog[12]` — actions 0–11 (Throttle, Strafe, Brake, SwitchPrimaryInventory, Yaw, Pitch, Roll, Peek, Lean, Fire, CameraPitch, CameraYaw) | `getLevel` `0x800460` |
| `+0x030`, `+0x158`, `+0x280` | three 74-entry per-action arrays (bookkeeping) | `0x8005c0/0x800610/0x800640` |
| `+0x3b8` | `u64` digital bits, bit = action − 12 (actions 12–73) | `setLevelBool` `0x800540`, `toggle` `0x800690` |
| `+0x3c0` | flags: bit0 **AuthoritativeAiming**, bit1 unknown | `0x800780/0x8007a0`, `0x800890/0x8008b0` |
| `+0x3c4` | `deltaTime` | `0x800720/0x800740` |
| `+0x3cc` | **authoritativeAimingYaw** (read as 0 unless bit0) | `0x8007c0/0x8007e0` |
| `+0x3d0` | **authoritativeAimingPitch** (read as 0 unless bit0) | `0x800810/0x800830` |
| `+0x3c8`, `+0x3d4` | unknown floats | |
| `+0x514` | frame tick | `0x800f90` |
`getLevel(0x80 /*EIAUndefined*/)` returns 0.

**How the server consumes it:**
- **Per-frame pass `0xb43230`** (controllable entity): for each entry whose player controls it and has `player+0x428`:
  humans are **skipped when single-stepping** (`vfunc 0x148`); **AI players (`+0x354`) are never skipped** — the engine copies
  the input, stamps the frame dt, and calls the soldier's apply-input vfuncs `0x150` / `0x154`. No input → `g_DefaultEntryInput`.
- **`ServerSoldierEntity::singleStepEntry(entryIndex)` = `0xb64890`** (vtable `0x27ba820` + `0x14c`) = VU's `soldier:SingleStepEntry`:
  `entries = soldier+0xc4`; `player = entry+0x4c` (must have `player+0x450 == soldier`); copy `*(player+0x428)`, scale dt;
  `vfunc 0x150`; **`CharacterPhysicsEntity::step(dt)`** (`soldier+0x1a4`, `0x49c880`); `vfunc 0x154`; dispatch `ProcessedInput`.
- **`shouldSingleStep()` = `0xb64800`** (`vfunc 0x148`): true iff not locally simulated, `soldier+0x3c5`, and `PhysicsSettings.SingleStepCharacter`
  (`+0x22`; in SP also `ForceSingleStepCharacterInSP` `+0x23`). The `ServerSoldierEntity` ctor (`0xb6b220`) stores it in `physics+0x117`.

**Implication:** an AI-flagged bot player with its own `EntryInput` at `+0x428` should be driven by the engine's normal
per-frame pass; calling `singleStepEntry` each tick (as fun-bots does) is a fallback if it isn't.

## Teams and kits (traced 2026-10-23, static)
- **`ServerPlayer::setTeam(teamId)` = `0xb2f2d0`** (thiscall, `ret 4`): no-op if unchanged; else `Player::setTeamId` (`0x801ed0`,
  writes `+0x38c`), tells the current soldier (`soldier->vfunc 0x130(team)`), then `onTeamChanged(new, old)` (`0xb2f090`: extent
  listeners + a team-changed message). The campaign spawner uses it to put AI players on the spawner's `Team`.
- **Team kit list:** `teams = *(*(g_ServerGameContext+0x10)+0x64)`; `teamEntityData = Teams::getTeamEntityData(teams, teamId)` (`0x7d3ca0`);
  `TeamData* td = teamEntityData+0x70` (`TeamEntityData.Team`). From the SDK, `TeamData`:
  `+0x0c Soldier` (SoldierBlueprint), `+0x10 CustomizedSoldier` (SoldierBlueprint), `+0x14 Faction`, `+0x18 TeamName`,
  **`+0x1c SoldierCustomization` (Array<SoldierCustomizationAsset> — MP kits are `VeniceSoldierCustomizationAsset`, `MPKitType` at +0x3c)**,
  `+0x20 VehicleCustomization`, `+0x24 UnitData`. FB2 arrays: pointer to element 0, count at `[ptr-4]`.
- **Three pieces of a loadout:**
  | Piece | Where | Set by | VU equivalent |
  |---|---|---|---|
  | Soldier blueprint | `player+0x4e0` | `setSelectedSoldierAsset` `0xb2f320` (`TeamData.Soldier`/`CustomizedSoldier` — which one the client sends: verify at runtime) | `CreateSoldier(blueprint)` arg |
  | Kit index | loadout state `+0x44` (`player+0x41c`) | `setKitIndex` `0xe3e1a0` (validated against `SoldierCustomization` count) | `selectedKit` |
  | Unlocks | `player+0x524..+0x528` (vector of `UnlockAsset*`; each `LinkedTo` applied) | — (writer not traced yet) | `selectedUnlocks` |
  `spawnSoldierAt` applies them via `applySoldierCustomization(player, soldier, 0, 0, 0)` (`0xb40120`), which uses the unlock vector
  when no explicit customization is passed.

## Server-side raycast (traced 2026-10-23, static)
- **`g_ServerPhysics = *(0x2aeaf88)`**, created by `0xa9daf0` during server startup (`0xaa97f0`), vtable `0x27acac8`.
- **`bool ServerPhysics::rayQuery(const char* ident, int mode, const Vec3* from, const Vec3* to, RayCastHit* hit, u32 flags, void* excluded)`
  = `0x7ac8e0`** (thiscall, 7 args). Returns true on hit. Builds a query (`0x7cf6f0`) and tests two internal collision structures.
- Reference use — `ServerTacticalSpawnPointChooserEntity::finalFilterVisibility` (`0xee4550`): for each living enemy, `from` = soldier
  position (`soldier+0x130` → `vfunc 0x0c` → transform `+0x30`) raised to eye height, `to` = spawn point, `mode = 1`,
  **`flags = 0x34`**, `excluded = 0`; a miss = visible.
- Flags line up with VU's `RayCastFlags` (`CheckDetailMesh 0x1, IsAsyncRaycast 0x2, DontCheckWater 0x4, DontCheckTerrain 0x8,
  DontCheckRagdoll 0x10, DontCheckCharacter 0x20, …`): `0x34` = no water/ragdoll/character; `CharacterPhysicsEntity::contactRayCast` uses `4`.
  fun-bots' LOS flags (`0x35`) map directly. **Verify bit meanings at runtime.**
- Generic interface path also exists: `physicsWorld->vfunc 0x68` → `IPhysicsRayCaster`, slot 0 = `physicsRayQuery(ident, from, to, hit, flags, excluded)`
  (BF3 signature) — used by `CharacterPhysicsEntity::contactRayCast` (`0x496ac0`).
- `RayCastHit` layout not mapped yet (bots mainly need hit / no hit; position for some checks).

## Damage arbitration (traced 2026-10-23, static)
- Server startup (`0xaa97f0`) **forces `SyncedGameSettings.AllowClientSideDamageArbitration = 1`** (`g_SyncedGameSettings = *(0x2ac5c0c)`, field `+0x1e`).
- **Soldier damage entry `0xb64fe0`** (`ServerSoldierEntity` vtable `+0x114`): if the damage is client-arbitrable (`damageInfo+0x6d & 8`)
  and the setting is on, it looks up the shooter's player (`0xbae090`) and **drops the server-side hit only when the shooter is human**
  (neither `isAI +0x354` nor `+0x424` set). **AI shooters' hits are applied by the server** (`soldier+0x6e8` handler).
- ⇒ Unlike VU/BF3, bots flagged `isAI` need **no** arbitration toggle around bullet collisions.
- Other readers (`0xdce900`–`0xdcfd02`, `0x960d70`, `0x99d480`, …) are weapon-firing/simulation paths keyed on the same flag.

## Putting it together — first bot (to verify at runtime)
1. `player = ServerPlayerManager::createPlayer(nullptr, -1, false)` (`0xb43a10`, `this = *(g_ServerGameContext+0x30)`), name it (`0xb3eb20`).
2. `setIsAIPlayer(true)` (`0xb2ada0`) → engine consumes its input every frame and applies its bullet damage server-side.
3. `setTeam(1 or 2)` (`0xb2f2d0`).
4. `setSelectedSoldierAsset(TeamData.Soldier)` (`0xb2f320`), `setKitIndex(k)` (`0xe3e1a0`), optionally fill unlocks (`+0x524`).
5. Construct an `EntryInput` (`0x80ef00`, ~0x530 bytes), set `AuthoritativeAiming`, store at `player+0x428`.
6. Spawn: dispatch `ServerPlayerForceRespawnMessage{+0x28 = player}` through `GameContext::dispatchMessage` (`0x5928c0`),
   or call `extent->spawnAt(player, &transform, 0, 1)` (`0xe1e9d0`) at an `AlternateSpawnEntityData` transform.
7. Each tick: write analog levels / digital bits / aim yaw+pitch; line of sight via `ServerPhysics::rayQuery`.

## Next steps
1. Start the DLL: proxy-load into `lanmohw.exe`, find a server-tick hook point, run steps 1–6 for one bot — first runtime test.
2. Map `RayCastHit` and confirm the raycast flag bits at runtime.
3. Trace who writes `player+0x524` (unlocks) if spawned bots come out without weapons.
