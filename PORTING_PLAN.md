# fun-bots → Medal of Honor: Warfighter — porting plan (v0)

Status as of 2026-10-22. Source of truth for "what fun-bots needs" is the code in
`refs/fun-bots` (Joe91/fun-bots @ `4ed81e3`, 114 Lua files, ~34.7k lines), not the
original brief. Everything below marked **verified** was checked against that code or
against GitHub; everything marked **unverified** still needs checking against the MOHW binary.

---

## 1. What's on this machine

| Item | Status |
|---|---|
| Battlefield 3 | `F:\EA Games\Battlefield 3\bf3.exe` — PE32 (32-bit x86) |
| Venice Unleashed | installed (`%LOCALAPPDATA%\VeniceUnleashed`), fun-bots 2.8.0 client UI bundle in `mods\` |
| MOHW retail install | **gone.** Origin manifest shows it was at `F:\Program Files (x86)\Origin Games\Medal of Honor Warfighter` (17.6 GB, offer `OFB-EAST:55174`); that folder no longer exists. Reinstall from the EA app. |
| MOHW LAN pack | `E:\mohw_client.exe` — 27 MB stub + ZIP payload (Jan 2023): `lanmohw.exe` and `MOHW.exe` (identical, 41 MB, PE32), `dinput8.dll`, `BFUlauncher.exe`, `Core/winhttp.dll`, `MOHW.par`. Extracting it into the game folder **overwrites `MOHW.exe`** — back up the retail exe first. |
| Other MOHW traces | ZLOEmu at `C:\ProgramData\ZLOEmu` (has `ZData/MOHW.zlo`); ProCon MOHW plugins at `D:\ProCon\Plugins\MOHW`; settings in `Documents\MOHW` |
| IDA | `D:\IDA Decompiler\IDAPortable` (has `ida.exe` + `ida64.exe`, so pre-9.0) |
| Visual Studio | 2022 Community 17.14 (for building the injected DLL) |
| Ghidra / x64dbg / ReClass.NET | not found |
| CMake, git, Python 3.14, Java | present |

---

## 2. Corrections to the original brief

| Claim in brief | What's actually true |
|---|---|
| fun-bots "performs server-side raycasts for line-of-sight" | **Wrong — verified.** Bot→player and bot→bot visibility checks are sent to *connected human clients* (`NetEvents:SendUnreliableToLocal('CheckBotBotAttack', …)`), run there with `RaycastManager:Raycast/CollisionRaycast` in `Client/ClientBotManager.lua`, and returned via `Botmanager:RaycastResults`. The server only raycasts for medic-revive checks (`BotManager.lua:1572`). **MOHW clients won't run a mod, so all LOS must move server-side.** This is the single biggest design change. |
| Bots move by writing `EntryInputState` each tick | **Incomplete — verified.** Writing input isn't enough: every tick fun-bots also calls `soldier:SingleStepEntry(player.controlledEntryId)` (`BotManager.lua:153`, commented "engine-requirement"). The server normally steps a soldier only when a client input packet arrives; bots have no client, so VU exposes a manual step. Finding MOHW's equivalent is the #1 reverse-engineering target. |
| Spawning = createPlayer + spawnSoldier | **Close — verified sequence:** `PlayerManager:CreatePlayer(name, team, squad)` → new `EntryInput` with `deltaTime = 1/tickrate`, `flags = AuthoritativeAiming` → `player.input = input` → `player:SelectUnlockAssets(kit, {appearance}, unlocks)` → `player:CreateSoldier(kit, transform)` → `player:SpawnSoldierAt(soldier, transform, pose)`. Bots are identified by `onlineId == 0`. |
| (not mentioned) | Bot bullets need **server-side damage arbitration.** Frostbite 2 trusts client hit reports; bots have no client. fun-bots hooks `BulletEntity:Collision` and temporarily sets `SyncedGameSettings.allowClientSideDamageArbitration = false` when the shooter is a bot (`Server/__init__.lua:589`). MOHW will need the same. |
| (not mentioned) | All 185 waypoint files in `mapfiles/` are BF3 maps — **none are usable for MOHW**. The node editor that records them is client-side UI. MOHW maps need a new, server-side way to record paths (see M6). |
| (not mentioned) | **fun-bots has no license** (GitHub reports none, no LICENSE file). Private use is fine; publishing a port of its code needs permission from Joe91 and contributors. |
| `BattleDash/Kyber` | Moved to `ArmchairDevelopers/Kyber`. Launcher is Dart; the C++ hooking code is in `Module/` (~1.8 MB C++). Frostbite 3, 64-bit — useful for architecture, not for offsets. |
| `PocketRelay/Tdf` | Doesn't exist. The crates are `jacobtread/tdf` and `jacobtread/blaze-ssl-async`. `PocketRelay/Server` is real and active. |
| `openBlase/openBlase` (C#) | Exists but is **C**, last pushed 2014. |
| `gosredirector` | No public repo by that name found. |
| `FrostyTypeSdkGenerator` | No such project. Frosty's SDK update lives in `FrostyPlugin/Windows/SdkUpdateWindow.xaml.cs` + `FrostySdk`, and targets Frostbite 3 TypeInfo layouts. For FB2, **`txt231/FBTools` is the better reference** — its README says it's tested on BF3 release (32-bit), and it's an IDA plugin, which you already have. |
| `astrelsky/Ghidra-Cpp-Class-Analyzer` | Real, but **archived** (2023). |
| ReClass.NET Frostbite plugin | Real, at `ReClassNET/ReClass.NET-FrostbitePlugin` (last push 2021). FB2/32-bit support unverified. |
| `Game.DisablePreRound 1` in Startup.txt | **Doesn't exist — verified.** `lanmohw.exe` has no `DisablePreRound` string. Pre-round is level logic: `PreRoundEntityData`, `ServerPreRoundEntity`, `ServerGameplaySetPreRoundLogicMessage`. If it hangs with bots, that's the path to look at. |
| `MOHWServer.exe` | The binary we have is `lanmohw.exe` (from the LAN pack): PE32, and it **contains server code** — `ServerPlayerManager`, `ServerSoldierEntity`, `ServerPlayer` (62 refs). Same model as BF3/VU: one client exe that can also host. |
| `ServerPlayerManager::createPlayer`, `m_isAIPlayer`, `MOHWSoldierCustomizationAsset` | **Still unverified names.** Confirm from the TypeInfo dump. |
| Verified real as stated | `Joe91/fun-bots`, `J4nssent/VU-Mods` (has `BotSpawn/`), `pedromartins1/BlazeServer` (C#, BF3), `grid-leak/blaze` (Rust, MEC), `CadeEvs/FrostyToolsuite`, `microsoft/Detours`, `TsudaKageyu/minhook`, `ThePhD/sol2`, `LuaJIT/LuaJIT` |

### Confirmed present in `lanmohw.exe` (string scan, 2026-10-22)
| fun-bots needs | MOHW string found |
|---|---|
| `SingleStepEntry` | `SingleStepCharacter`, `ForceSingleStepCharacterInSP` |
| `allowClientSideDamageArbitration` | `AllowClientSideDamageArbitration` |
| `EntryInputFlags.AuthoritativeAiming` | `AuthoritativeAimingActive` |
| `EntryInputActionEnum` / `EIAThrottle` | both present (reflected enum → exact values recoverable) |
| `SoldierBlueprint`, customization | `SoldierBlueprint`, 9× `*CustomizationAsset` |
| Reflection data | dedicated PE sections `typeinfo` and `fieldinf` — makes a static TypeInfo dump much easier |

Other sections: `.text, ctr, .rdata, .data, .idata, typeinfo, fieldinf, .tls, .rsrc, .666` — `ctr` and `.666` look non-standard (likely added by whoever patched the LAN build); check before trusting `.text` as pristine.

---

## 3. The engine surface fun-bots actually uses (verified)

### Tier 0 — a bot exists and walks a path
| VU API | Frostbite primitive to find in MOHW |
|---|---|
| `PlayerManager:CreatePlayer(name, team, squad)` / `DeletePlayer` | ServerPlayerManager: create/destroy a player with no network connection |
| `EntryInput()` + `.deltaTime`, `.flags = AuthoritativeAiming` | EntryInputState struct layout + flags field |
| `input:SetLevel(EIA*, value)` / `GetLevel` | per-action float array in EntryInputState |
| `input.authoritativeAimingYaw/Pitch` | aiming fields used when AuthoritativeAiming flag is set |
| `player:SelectUnlockAssets(kit, {appearance}, unlocks)` | kit + unlock selection on ServerPlayer (MOHW's class/unit system differs from BF3) |
| `player:CreateSoldier(kit, transform)` / `SpawnSoldierAt(soldier, transform, pose)` | ServerPlayer soldier creation + spawn |
| `soldier:SingleStepEntry(controlledEntryId)` **every tick** | the server-side "consume one input frame and simulate" call |
| `UpdateManager:Update` (PostFrame pass) | a per-tick hook on the server update loop |
| `player.soldier`, `.worldTransform`, `.teamId`, `.squadId`, `.id`, `.name`, `.onlineId` | ServerPlayer / ServerSoldierEntity fields |

Input actions used (enum *values* will differ in MOHW — read them from the reflected `EntryInputActionEnum`):
`Throttle, Strafe, Yaw, Pitch, Roll, Brake, Fire, Sprint, Jump, QuicktimeJumpClimb, Interact, Zoom, Reload, MeleeAttack, QuicktimeFastMelee, SelectWeapon1–7, ToggleParachute, FireCountermeasure, CycleRadioChannel, CameraYaw, CameraPitch`.

### Tier 1 — a bot fights
| VU API | Frostbite primitive |
|---|---|
| `RaycastManager:Raycast` / `CollisionRaycast` (currently client-side) | **server-side** physics raycast (Havok world query) |
| `Hooks:Install('BulletEntity:Collision')` + `SyncedGameSettings.allowClientSideDamageArbitration` | bullet collision handler + damage-arbitration setting |
| `Hooks:Install('Soldier:Damage')` | soldier damage function (shoot-back trigger, damage multipliers) |
| `soldier:Kill()`, `SetPose`, `SetTransform`, `.corpse`, `HealthAction` | soldier entity methods |
| Events: `Player:Killed`, `Player:Respawn`, `Player:Left`, `Level:Loaded`, `Level:Destroy`, `Server:RoundOver/RoundReset` | game-event hooks for lifecycle |

### Tier 2 — defer
Vehicles (`EnterVehicle/ExitVehicle`, `controlledControllable`, `Vehicle:*` events, 5 `Vehicle*.lua` files), capture-point/objective events (`GameDirector.lua`, 2.2k lines), `EntityFactory:Create` (flare/smoke vs missiles), `ResourceManager` EBX lookups (all BF3 asset paths), SQL settings DB, WebUI, node editor, chat/RCON commands, 9 language files.

### Size of what's worth porting
Infantry brain ≈ 8–9k lines: `Server/Bot/` minus vehicle files, `Server/BotStates/` (1.2k), core of `BotManager.lua` (1.7k), `NodeCollection.lua` (1.9k), `PathSwitcher.lua` (0.3k). Everything else is UI, vehicles, BF3 asset tables or tooling.

---

## 4. Recommended approach

Neither of the brief's pure options:
- **Full Lua bridge (Option A)** means emulating ~all of VU — events, NetEvents, typed EBX wrappers, SQL, UI. Too large.
- **Full native rewrite (Option B)** throws away ~9k lines of tuned bot behaviour.

Instead: **native C++ core + embedded LuaJIT running a trimmed fun-bots brain.**
1. A C++ DLL (MinHook, VS 2022, x86 if MOHW is 32-bit) owns everything engine-facing: create/delete player, spawn, input write + `SingleStepEntry`-equivalent, server raycast, damage arbitration, per-tick hook.
2. It exposes a small VU-*shaped* Lua API (`PlayerManager`, `EntryInput`, `RaycastManager`, `Vec3`, `LinearTransform`, a handful of `Events`) via sol2 — only the Tier 0/1 surface above.
3. Port the infantry brain into that, replacing the client-raycast round-trip with direct server raycasts and stubbing UI/vehicles/SQL.

**Use BF3 as the Rosetta stone.** You have `bf3.exe` (32-bit FB2, and VU drives the same engine code). Find each primitive in `bf3.exe` first — where FBTools is already tested and VU's behaviour tells you what "correct" looks like — then locate the same function in MOHW by shared strings, TypeInfo names, and call-graph shape. Do the engine RE on the game binaries only, not on VU's own (closed-source, separately licensed) DLLs.

---

## 5. Milestones

| # | Goal | Done when |
|---|---|---|
| M0 | MOHW server running locally | you can join your own server with a client; note exe name, bitness, Startup.txt keys (check `DisablePreRound` in strings) |
| M1 | TypeInfo dump | FBTools builds against your IDA SDK; dump of `bf3.exe` works (validation), then MOHW dump → C++ headers |
| M2 | Primitives located in BF3, then MOHW | addresses/signatures for: create player, select unlocks, create+spawn soldier, step entry, per-tick update, server raycast, bullet collision, damage-arbitration setting |
| M3 | First native bot | injected DLL spawns one bot that walks forward and turns (hard-coded input), no Lua |
| M4 | Bot shoots and hits | server raycast LOS + fire input + server-side damage applied to a human |
| M5 | Lua brain | LuaJIT + sol2 shim; trimmed fun-bots infantry logic drives bots on a hand-made path |
| M6 | MOHW waypoints | server-side recorder: a human walks the map, chat commands start/stop/mark, server samples their soldier position into fun-bots' `.map` format (`pathIndex;pointIndex;x;y;z;inputVar;json`) |

---

## 6. Open questions (need your answers)

1. ~~Where are the server files?~~ Answered: the LAN pack at `E:\mohw_client.exe`; game data needs a reinstall from the EA app.
2. ~~32- or 64-bit?~~ Answered: 32-bit, same as `bf3.exe`.
3. Do you have a working Blaze/backend emulator for MOHW already, or is that part of this project too? (ZLOEmu is installed — was that what you used to host?)
4. Private use only, or do you intend to publish the port? (Affects the fun-bots licensing question.)
