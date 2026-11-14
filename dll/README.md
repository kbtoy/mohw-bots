# mohw-bots DLL — first test build

An `xinput1_3.dll` proxy that loads into `lanmohw.exe`, hooks `Server::update`, and spawns AI players with MOHW's own
pipeline (see `re/RE_NOTES.md`, "Putting it together"). This build only proves the plumbing: create a bot, spawn it,
make it walk. No brain yet.

## Safety
- On load it checks the first bytes of all 18 engine functions it uses (`src/signatures.inc`, generated from
  `lanmohw.exe` by `tools/gen_signatures.py`). Any mismatch (other build, or the retail exe whose code is still
  encrypted at load time) → it logs and does nothing except forward XInput.
- Every engine call and engine memory read is wrapped in SEH; a fault is logged instead of crashing.
- Bots are dropped automatically when their player disappears (level change).

## Build
```bash
cmake -S dll -B build -G "Visual Studio 17 2022" -A Win32
```
```bash
cmake --build build --config Release
```
Output: `build/Release/xinput1_3.dll` (32-bit, static CRT; imports only KERNEL32/USER32).

Offline checks against the real game code (no game launch). First dump the exe's memory image once:
```bash
tools/.venv/Scripts/python.exe -I tools/dump_image.py re/lanpack/lanmohw.exe build/lanmohw.image.bin
```
```bash
cmake -S tools/smoketest -B build/smoketest -G "Visual Studio 17 2022" -A Win32
```
```bash
cmake --build build/smoketest --config Release
```
```bash
build/smoketest/Release/imagetest.exe build/lanmohw.image.bin build/Release/xinput1_3.dll
```
It maps the game image at its real addresses, loads the DLL, and checks the signature match, the hook bytes, and the
`EntryInput` layout by calling the engine's own constructor and setters.

## Install
1. Make sure the LAN pack is applied to `F:\Medal of Honor Warfighter` (its `MOHW.exe`/`lanmohw.exe` are the
   decrypted build these addresses come from). The retail exe is backed up in `I:\MOHW Modding\retail-backup`.
2. Copy `build/Release/xinput1_3.dll` and `dll/mohw-bots.ini` into the game folder (next to `MOHW.exe`).
3. Start and **host** a LAN game the way you normally do; the bots run on the hosting machine's server.

## Test
**Without a backend (works today):** start `lanmohw.exe` directly — with no arguments it adds `-webmode spoffline`
and runs offline. The campaign runs a local server in-process, so the hook ticks there. Start or continue a mission,
wait until you control your soldier, then use **F7** (spawn in front of you). F8 uses the multiplayer spawn-point
chooser, which may not exist in single-player.

**MP map, offline:** set `[launch] Map` in `mohw-bots.ini` (shipped `MP_05,MP_03`: the two take turns, one per
launch, the next remembered in `mohw-bots-state.ini`; `Mode` picks the game-mode layer, Team Deathmatch by default)
and start `lanmohw.exe` with **no arguments**. The DLL builds `-level MOHW/Levels/MP/<Map>/<Map>
-Game.DefaultLayerInclusion GameMode=<Mode>` (+ `StartupArgs` as extras) and `LayerInclusion` = `GameMode=<Mode>`.
Maps with a TDM layer (`<map>_tdm` bundle): MP_03, MP_05, MP_10, MP_12, MP_13, MP_16, MP_18, MP_19 (the Xpack1 update
adds MP_20 / MP_21). Their names are hashed in the string tables, so once in game the DLL logs the map's name from the
game's own text (`0xEC0B50` localize(out, "ID_M_LEVEL_NAME_<code>") -> wide string, released by `0x5152E0`):
`map: MP_05 = "..."`. A new map starts with an empty nav graph (bots walk straight at enemies until routes are learned).
**Teams:** `[bots] TeamSize=10` fills both teams with bots once you have a team - you count on yours, so you + 9
against 10 - one bot every 0.4 s, quietly (no key beep). The offline server allows 64 players. Why not a command
line:
- With an empty command line the game appends `-webmode spoffline` itself (the LAN build's "offline launch?" check at
  `0x410f30` is patched to always return true) and runs offline with a local server.
- Any real argument skips that: `-level …` alone starts online mode with no local server (no `first Server::update
  tick`, so hotkeys do nothing), and `-webmode spoffline -level …` makes the game start the Origin SDK, which launches
  the EA app and exits.
- So the DLL rewrites the operand of that built-in append (`push` at `0x418182`, signature-checked) to
  `-webmode spoffline <StartupArgs>` before WinMain runs. Clear `StartupArgs` to get the normal menu back.
- An MP startup level still starts the Origin component, whose init (`0xDC41F0`) reads
  `VeniceOnlineSettings.EnableOrigin` (+0x6C) and, if set, runs `OriginStartup` → EA app → exit. `DisableOrigin=1`
  replaces that read (`0xDC4211`, `8A 40 6C` → `32 C0 90`) so the component takes its "Origin disabled" branch.
  Keep it 0 if you use the LAN pack's emulated Origin through BFUlauncher.
- The MP level's server then picks a backend in `0xD257B0` (SP/coop → `Backend_Local`; otherwise the configured
  `OnlineSettings` backend) and creates it through a factory table (`0xE9EA00`, table `0x2B14904`). Every crash dump
  shows type **1 = `Backend_Blaze`** (`EDX = EDI = 1`), whose factory isn't registered offline (only Local and Peer
  register via `0xE9E9D0` here), so `0xE9EA15` calls null ~7 s after start — every `-level MP_xx` launch did this.
  `ForceLocalServerBackend=1` turns the first branch (`0xD257BF`: `75 70` → `EB 70`) into an unconditional jump to
  the existing `mov edi, 3`, so every server uses `Backend_Local`. (An earlier attempt retargeted only the
  `IsPresenceEnabled == false` → `Backend_Lan` branch; the crash wasn't on that branch.)

- With the level now loading (message trace: `ServerInternalLoadLevel` at ~6.8 s, `ServerLevelLoaded` at ~20 s), the
  client connected at ~6.8 s but was disconnected at ~14.3 s → returned to a menu that was never loaded → black
  screen. Cause: `ClientSettings.Loading/Loaded/IngameTimeout` and `ServerSettings.LoadingTimeout` default to **5 s**,
  and the client copies them into its connection when it connects (`0x878CD0`; true local connections instead get
  172800 s). `ConnectionTimeoutSeconds=300` raises them through `SettingsManager::getContainer` (`0x58AA60`) from the
  first XInput poll (~4 s, main thread), before the connection exists.
- That alone didn't stop the disconnect (~6 s after the client reached `Static`, while the server was still loading).
  Both peers also set a transport-level "no packets" timeout from `NetworkSettings.ConnectTimeout` (+0x40, client
  `0x887DD0`, server `0xB0B280`, both through `0x618A80`); the server pumps no packets during its ~13 s load, so
  `ConnectionTimeoutSeconds` now raises that one too.
- That didn't change it either: `ConnectTimeout` was 5 s and raised to 300 s before the connection existed, yet the
  client still dropped ~6.2 s after `Static`, with code **16** and an empty text. Taking `SecureReason`'s values in
  declaration order, 16 is `SecureReason_NoReply`; the exit-to-menu reason was 3 (`UnableToConnectToServer`). So it's
  not an inactivity timeout. Next: the disconnect backtrace (below) shows which code raises it.
- The backtrace: transport peer update (`0x627B1D` → `0x6273B1`) → connection update `0x61D670` → state-change
  notify `0x613830` → listener (`0x87BF96`) → `ClientConnection::onDisconnected(16, "")`. The transport connection
  was in state 13, "Disconnected by remote", which `0x620210` sets when a type-0xE packet (8-bit code + text)
  arrives. So the **local server** kicked the client with `SecureReason_NoReply` (16; the enum's names sit in
  declaration order at `0x2766AFC` downward). The server sends that packet through `NetPeer::disconnect`
  (`0x61FE90`: connection, code, text), which is now hooked too, with a backtrace from the server's side.
- **Root cause (that guess was wrong):** the server never sent anything. `NetPeer::disconnect` only fired *after* the
  client's disconnect, from the client's own cleanup (`0x873DE0` echoing code 16). The listener at `0x87BF96` is
  the client peer's "peer error" callback (`0x87BEC0`), which hands the peer's stored error (`peer+0x1950`, text
  `+0x1954`) to every client connection. That error is 16 from `NetPeer::setErrorNoReply` (`0x61F790`, peer vtable
  `0x2765EA8` slot 9), called when a transport connection reaches state 8/9 ("Challenge Sent - No reply" /
  "Challenge Failed!"). In `TransportConnection::update` (`0x61D670`) the challenge states resend every 0.5 s and give
  up after **15** resends: 7.5 s, which matches every run (client starts at 6.58 / 6.66 / 7.20 s, drops at
  14.16 / 14.14 / 14.67 s). The local server can't answer the challenge while its thread is busy loading MP_03
  (~13.5 s). `ChallengeRetries=120` raises both limits (`0x61D6EA` / `0x61D703`, the imm8 of
  `cmp dword [esi+0xA0], 0x0F`) to 60 s. The trace now also logs every transport state change
  (`transport: connection … Send Challenge -> Connected after N resends`).
- With 120 retries the client knocked for exactly 60 s, and the **server's load also took 60 s longer**: it finished
  ~5.7 s after the client gave up and unloaded, in every run. So it's a deadlock, not a slow server: the server's
  level load can't finish while the client sits connecting over UDP to its own process. How the client connects is
  decided by `ClientStartMultiplayerMessage` (handler `0x8A68C0`): `+0x44` → `client+0x1F1` → connection mode 0
  (single-player style), `+0x45` → `client+0x1F2` → mode 2, neither → mode 1 (UDP transport). Modes 0/2 use an
  in-process link (`0x588990`, created in `0x878E40`) instead of a transport connection. The `-level` startup path
  (`0x89D130`) sends the message with both flags clear; the game's own front-end start (`0xDD75E9`,
  `Levels/FrontEnd/FrontEnd` as an MP level with an in-process server) sets `+0x45`. `LocalClientLink=1` makes the
  handler always store 1 there (`0x8A69A1`: `setne al` → `mov al, 1; nop`), so the client links to its in-process
  server the way the front end does.
- With that, the client connects in ~2 s (still a transport connection, but over the in-process link: challenge →
  Connected at 8.5 s), the server loads MP_03 by 16.7 s, the client goes in-game, and the server creates the
  player (F11: 1 player, team 0, no soldier). But the level has **0 teams**, there's no deploy screen, HUD or menu,
  and the camera is the pre-spawn free camera. The `GameMode` sub-level isn't loaded: MP_03 includes
  `MOHW/Levels/MP/MP_03/MP_03_TDM` (and others) only for criterion `GameMode` (defined in
  `MOHW/Settings/LevelLayerInclusion`, options TeamDeathMatch, CombatMission, CaptureTheFlag, SectorControl, Sport,
  BombSquad, ObjectiveRaid, …), and that bundle holds the teams (`gameplay/teams/teamallies`, `blueonblue_teams`),
  the spawn layer and the game-mode logic. Every `ClientStartMultiplayerMessage` sender builds its LevelSetup's
  inclusion options from `GameSettings.DefaultLayerInclusion` (`0x7E7E20` → `0x7E7D70`, `"Criterion=Value;..."`);
  the in-process server loads the level from that setup. `[launch] LayerInclusion=GameMode=TeamDeathMatch` writes
  it into GameSettings directly (first XInput poll, before the message is built) instead of relying on the
  `-Game.DefaultLayerInclusion` argument; the log shows the value the game had, the criteria table, and the
  inclusion options the start message actually carries.
- **That worked:** the game's own `DefaultLayerInclusion` was empty (the `-Game.DefaultLayerInclusion` argument is
  never applied), the start message now carries `GameMode=TeamDeathMatch`, the level gets 3 teams (neutral + 2), the
  deploy screen works (class and weapon pick, spawn countdown), and **F8 spawns a bot** (team 1, kit 0 of 6, soldier
  at an engine-chosen spawn point). But the human player stays on **team 0**: online, matchmaking assigns the team and
  TDM waits for it (`pf_waitforteamexternal`); offline nothing does, so after the countdown the server unspawns the
  customisation soldier and the spawn sits at "Falling back". `[bots] HumanTeam=1` puts any human still on team 0
  onto team 1 (`ServerPlayer::setTeam`) as soon as the level has that team; bots then default to team 2.
- **That worked too:** the player spawns (team 1, asset = TeamData `Soldier`), the ESC menu works, and 8 bots spawned
  on team 2 with soldiers at engine spawn points and in front of the player. But the bots were **invisible and took no
  damage**. They used TeamData `CustomizedSoldier` (the first build's default), while the game spawns humans with
  `Soldier`; `CustomizedSoldier` looks like the deploy screen's preview body (the server spawns and unspawns one
  around `NetworkSpawnCustomizationMessage` / `NetworkUnSpawnCustomizationMessage`). Blueprint 0 now means `Soldier`.
  (Ruled out: the per-soldier "force invisible" flag — `NetworkSoldierForceMeInvisibleMessage` → handler `0xB42684`
  → `ServerSoldierEntity` vtable `+0x184` = `0xB67D40`, flag `soldier+0x61C`, which also moves the physics body — is
  cleared in the constructor `0xB6B3CF`.) F6 adds a bot on the player's team.
- `Soldier` bots were **still invisible**, though the player could spawn on one as a fireteam buddy (so the server
  soldier and its replication work). The body comes from unlocks: a kit (`SoldierCustomizationAsset`, from
  `TeamData.SoldierCustomization`) has a `VisualTable` (+0x18) and `WeaponTable` (+0x24), each a list of
  `CustomizationUnlockParts` with `SelectableUnlocks` / `AutomaticUnlocks`. The client picks unlocks in the
  customisation screens and sends `NetworkPlayerSelectedUnlockAssetsMessage`; the server handler `0xB41B70` refills
  `ServerPlayer+0x524` (an `eastl::vector<UnlockAssetBase*>`), and spawn customisation `0xB40120` builds the soldier
  from it. Bots had an empty list, so no body (and presumably no hit boxes). Each new bot now gets the first selectable
  unlock of every part of its kit's two tables, pushed with the engine's own `vector::insert` (`0x987A40`); the log
  lists them (`default unlocks from kit 0: N (...)`).
  The first version crashed on F6: `0x987A40` is `DoInsertValue`, whose in-place path assumes position < end. Called
  at position == end on a vector with room (a new player's 32 inline slots at `+0x538`), it memmoves
  `end - 4 - position` = -4 bytes and wrecks the heap (another thread then faulted in the allocator at `0x556618`).
  The game's handler appends in place while there's room and calls `0x987A40` only when full (growth path); the DLL
  now does the same, and the image test pushes 32 unlocks onto a fake player to check it.
- With that, F6 **froze** the game: the bot got 7 unlocks, all weapons/gadgets (TAC-300, Glock 18, switchblade,
  proximity mine, tomahawk, two support actions), and `ServerPlayerForceRespawnMessage` faulted mid-spawn; SEH caught
  it but left the engine stuck (presumably a lock held). Kit_Sniper (`mohw/gameplay/kits/kit_sniper`) shows why the
  list was odd: its VisualTable has only an **automatic** unlock (the kit unlock, `sniper_kit_unlock`), its
  SpecializationTable only an automatic soldier appearance (`soldiers/visual/mp/backup/co_assault_backup`), and the
  WeaponTable 10 parts. Bots now get every automatic unlock plus the first selectable one from all three tables
  (`[bots] Unlocks`: 0 none, 1 no weapons, 2 all). Engine faults now log the faulting instruction, access address,
  registers and game return addresses, and F11 lists every player's unlocks by name.
- F8 still froze; the fault log showed why: spawn customisation (`0xB40196`) reads `+0x24` of **every** entry in
  `+0x524` as `UnlockAsset.LinkedTo` and calls each element's `getType` (`0xB401C8`). The kit and appearance entries
  are `UnlockAsset`s, but the weapons are `SoldierWeaponUnlockAsset`s (a sibling class whose `+0x24` is the `Weapon`
  blueprint), so it called through garbage (`EIP = B855F8B8`, return `0xB401CC`). Weapons live in a separate set:
  the unlocks handler `0xB41B70` takes them from `msg+0x4C` into `ServerPlayer+0xD4` (flag `+0x3E0 |= 0x20`), and when
  the client sends none it falls back to the first `SoldierWeaponUnlockAsset` of each part of the selected kit's
  WeaponTable (`msg+0x38` → `+0x24`, class check against `TypeInfo(SoldierWeaponUnlockAsset)+0x14` = `0x2B81218`).
  Bots now only get entries that pass the game's own `isKindOf(UnlockAsset)` test (`eng::IsKindOf`: `getType()` then
  the class-id range at TypeInfo `+0x14/+0x16`); weapons are skipped and logged. Next: weapons through that handler.
- That ran clean: 11 bots (F6/F7/F8) spawned with `CO_Assault_Backup, Sniper_Kit_Unlock`; no fault. For comparison
  the player's list was `CO_DemoOGA_Desert_1P, CO_DemoOGA_Desert` (3rd- and 1st-person body of the chosen unit; the
  kit and weapons live elsewhere). No bot died (no `ServerPlayerKilledMessage`).
- **Weapons (and the game's own default appearance):** `0xB41B70` with a message that has a kit but no appearance and
  no weapons is the game's default-loadout path: `0x805CE0` stores the kit (`ServerPlayer+0xD0`, refcounted); for
  every WeaponTable part it builds a `NetworkPlayerSelectedWeaponMessage` (`0x25E5FA0`) from the first
  `SoldierWeaponUnlockAsset`, adds default attachments (filtered by the player's vtable `+0x28` "unlocked?" check),
  and runs the weapon handler `0xB41A10`; then it builds a new unlock message (`0x25E5E30`, 0x60 bytes) for the kit,
  fills default appearance with `0xB3E810` (VisualTable walk) and calls itself with it. `eng::ApplyDefaultLoadout`
  does exactly that from the DLL (ctor, kit at `+0x38` with a reference added, handler, then the game's cleanup:
  `0x5634B0` on both arrays, `0x564300` on the kit, `0x6284E0`). `Unlocks=2` uses it and falls back to the kit's own
  UnlockAssets if the bot ends up with no appearance (the recursive call filters through the player's vtable `+0x2C`).
- That worked (no fault; each bot ends with `CO_Assault_Backup`) and bots are **visible** now, but they stand with
  their feet in the ground and **can't be damaged**: no `ServerSoldierSoldierDamageMessage` for any bot, and even a
  grenade (server-side damage) only hit static models. In MP, soldiers are single-stepped (`shouldSingleStep`
  `0xB64800`: not locally simulated, `soldier+0x3C5`, `PhysicsSettings.SingleStepCharacter`): the character's input
  and physics (`CharacterPhysicsEntity::step`) only run in `singleStepEntry` (`0xB64890`), once per input frame —
  per network packet for humans, by the AI system for AI. MP levels have no AI system, so bot soldiers were never
  stepped: no ground snap, no live physics body to hit. `Drive` now stamps the tick's dt into the bot's input
  (`EntryInput+0x3C4`; `singleStepEntry` steps by input dt × time scale) and calls `singleStepEntry(0)` through the
  soldier's vtable (`+0x148` / `+0x14C`) every server tick, as fun-bots does with `SingleStepEntry` in VU. (The
  engine's per-frame pass `0xB43230` also applies AI input via `0x150`/`0x154`, so input is applied twice per tick;
  watch for side effects once bots shoot.)
- **Result: bots die**, the player can change class and respawn, no engine faults in a 14-minute session. But every
  bot logged "soldier isn't single-step (physics world steps it)", so `singleStepEntry` never ran: the fix was the
  **input deltaTime**. The per-frame pass applies an AI player's input with the input's own `deltaTime`; ours was 0,
  so the character never moved or settled. `StepSoldier` stays as a no-op fallback for single-step soldiers.
- **Bot death = player deleted:** a killed AI player is destroyed (`ServerPlayerKilled` → `ServerPlayerAboutToClearSoldier`
  → `ServerPlayerReleasingLevel` → `ServerPlayerDestroy`), as for campaign AI, so the old "force-respawn the same
  player" never fired. Now a bot whose player was destroyed is queued and re-added after `AutoRespawnSeconds` with
  the same name and team at an engine spawn point (queue cleared on level change).
- **Crash on suicide/respawn:** `0xE75637` read `[g_0x2B14928 + 0x3C]` with the global null. `0x2B14928` is an online
  stats object (constructor `0xEA60E0`, created from `0xE22100`), never created offline. Most users null-check it; three
  don't: the kill handler (`0xE75631`), an awards dispatcher (`0x169BB40`, from scoring `0xE7F420`), and the
  deploy-stats handler (`0x169C680`, `NetworkPlayer(FirstTime)DeploymentMessage`). `guards.cpp` (always on) sends the
  kill handler to a stub that takes its existing "no stats" exit (`0xE7579A`) when the object is missing, and makes
  the other two return early.

## Bot brain (`src/brain.cpp`, `[brain]` in the ini)
A native port of the fun-bots infantry logic (`refs/fun-bots/ext/Server/Bot/*`, `BotStates/*`) onto MOHW's
`EntryInput`; `bots.cpp` builds a snapshot of every living soldier each server tick (`Actors`) and calls
`brain::Update` per bot. Conventions are fun-bots': `yaw = atan2(dz, dx) − π/2` in [0, 2π) (0 faces +Z,
forward = (−sin, 0, cos)), `pitch = atan2(dy, horizontal)`, eye 1.6 m, chest 1.1 m, head 1.65 m above the feet.
- **Perception:** every 0.15–0.25 s (staggered), line of sight from the eye to the chest of the current target, else
  of the 3 nearest enemies in range, via `ServerPhysics::rayQuery` (`0x7AC8E0`, `*0x2AEAF88`, mode 1, flags `0x34`:
  no water / ragdoll / characters — the spawn chooser's own visibility test). A visible target is tracked every tick.
  Enemies outside the `FovDegrees` (120; fun-bots uses 160) cone are ignored unless within `HearingMeters` (5 m); one
  noticed behind the bot gets 0.4–0.8 s extra reaction (time to turn round), and a current target stays tracked
  wherever it goes.
- **Aim:** turn toward the target at `TurnDegreesPerSecond` (fun-bots `UpdateYaw`), with an error of
  `AimErrorCentimeters × (1.5 − skill) / (distance + 10 m)` (fun-bots aim worsening; the + 10 m keeps the angle small
  up close, where `/ distance` made bots spray wide of each other at 3–10 m), drifting sway plus jitter and recoil (see
  below); `AimForHead` aims at the head, snipers do about `SkillPercent` of the time (rolled per target).
- **Fire** (analog `EIAFire` = 9): only after a reaction delay and within 0.12 rad of the wanted aim; bursts by kit
  (`MPKitType` at kit `+0x3C`): sniper single taps 1.2–1.8 s apart, support 0.6–1.2 s bursts, others 0.3–0.6 s bursts
  (taps beyond 40 m). Reload (digital `EIAReload` 18) after 2.5 s of trigger time, when out of combat.
- **Attack movement** (fun-bots `StateAttacking`): every 2–4 s decide stop-to-shoot (always for snipers, else 40 %)
  or strafe, switching side every 0.7–1.6 s; close in on targets beyond 35 m. Lose sight for 2.5 s → go to the last
  seen position.
- **Roaming** (no waypoints yet): head for the nearest enemy (± a few metres; random point if none), re-picked every
  6–10 s, looking where it walks and sprinting beyond 20 m. Obstacles: rays 1.4 m ahead at 0.45 m and 1.2 m — knee
  blocked only → jump; chest blocked → first clear direction of ±0.8/1.6/2.4 rad (2.5 m ray), held 0.8–1.4 s. Stuck
  (< 0.6 m moved in 1.5 s) → jump and a random detour. Movement is converted to throttle/strafe relative to the aim.
- **Grenades** (digital `EIAThrowGrenade` 29): when the target ducks out of sight for 0.5–2 s at 8–25 m, a
  `GrenadePercent` (35 %) chance per disappearance to aim at its last position, pitched up by distance (a lob table),
  line up for 0.35 s, then hold the throw input for 0.25 s; 15–25 s cooldown. Logged as `grenade at the enemy's cover`.
- **Kits:** `KitIndex=-1` gives each bot a random kit; the game's default loadout for that kit supplies its weapons.
- **Diagnostics:** `brain <bot>: engaging …`, `lost sight …`, `stuck …`, and a one-off `aim check` per bot comparing
  the input yaw with the soldier's actual facing (confirms the yaw convention in MOHW).
- **First run:** the yaw convention checked out (`aim check`: input yaw 2.86/3.42/3.05 = soldier facing 2.86/3.42/3.05),
  bots spotted and engaged the player and each other, but **never walked** (positions unchanged for minutes, constant
  "stuck"): `StepSoldier` only stepped when `shouldSingleStep()` was true, which it never is for bot soldiers, and
  nothing else runs their character physics (aim still applied through the per-frame pass). fun-bots calls
  `soldier:SingleStepEntry` for every bot every tick, unconditionally ("engine-requirement"); `[bots] AlwaysStep=1`
  does the same.
- **Crash when a bot killed the player:** the client "killed by" panel (`0x17D57F0` → `0x17D50E0`) looks up the
  killer's persistence record with `0x9075D0` and reads `[record+0x160]` unchecked; bots have no record (null).
  `guards.cpp` sends `0x17D5123` to a stub that takes the function's own exit (`0x17D51D6`) when null. The other
  unchecked callers (`0x16AE259/3D7/607`) look up the local player, which always has one.
- **With AlwaysStep bots walk** (13 "stuck" lines in a run instead of one per bot every 1.5 s; Bot08 went from
  (101.9, 70.5, 69.0) to (61.2, 69.8, 29.1)) and fight each other. **New crash: spawning on a bot fireteam buddy.**
  The client teammate check `0x95ECC0` (your soldier against an overlapping teammate) takes an AI-only branch that
  reads `clientLevel+0x60` (LevelData) `+0x4C` (`AISystem`) `+0xC` unchecked; MP levels have no AISystem (it was
  never reached before because bot soldiers had no live physics). `guards.cpp` null-checks it and continues at
  `0x95EDAB`, the code's own "no AI setting" path. A scan of every `[x+0x60] → [+0x4C]` read found no other
  unchecked AISystem use (`0xAA8C2B` server startup passes it on; the rest are vtable calls or null-checked).
- **First full match** (smooth, no crash): 62–59 TDM, bots fight and score. Findings:
  - **0 points, support-action bar never filled** — caused by the first awards guard: `0x169BB40` awards the score
    first (`vtable+4` → `0x169AD40`) and only its award loop asks the stats object (`0xEA7A90`). Skipping the whole
    routine threw away every score award. Now only the loop read (`0x169BBCE`, `mov ecx, [0x2B14928]`) is guarded:
    a stub replays it and leaves the loop at its own exit `0x169BC8D` when null.
  - **Enemy bots too accurate:** bot shots in MOHW don't get recoil/climb, and the old error held one offset for
    0.25–0.45 s, so whole bursts landed together. Now: sway drifting toward offsets re-rolled every 0.2–0.4 s,
    per-tick jitter while firing, simulated recoil climb (`RecoilPercent`, 0.10 rad/s up to 0.05 rad, `1 − 0.6 ×
    skill` of it uncorrected), ×1.5 error while strafing; defaults `AimErrorCentimeters` 45 → 90, `SkillPercent` 60 → 50.
  - **Perf:** server at 59 Hz (17 ms ticks) with 9–12 bots; bot work 0.76–0.90 ms average per tick (max 2.4–6.4 ms),
    155–276 rays/s — about half of it the engine's own character step per bot.
  - Friendly bots stuck in the spawn building at first: no navigation data yet (see below).
- **Performance:** `perf:` lines once a minute (bot work per server tick, rays/s, tick rate); settings overrides now
  re-check every 2 s once applied instead of on every XInput poll.
- **Second round (navigation on, several matches):** the graph grew to 519 nodes / 1299 links on MP_03; perf 0.53–0.60
  ms of bot work per tick at 59 Hz with 8–9 bots (one 24.6 ms spike in a minute). 971 `stuck` lines, clustered in a
  few 10 m cells (the spawn buildings), with bots running in circles: the detour after "stuck" was a random direction,
  often back into the same room, and the graph only had rooms the player had walked through. Feedback and changes:
  - **Bots missing each other at ~3 m:** the `/ distance` error curve → `/ (distance + 10 m)` (above).
  - **Enemies turning round as soon as you're behind them:** perception had no field of view (the LOS check is
    omnidirectional). Now `FovDegrees` + `HearingMeters` + extra reaction from behind.
  - **No grenades:** added (above).
  - **Stuck in rooms:** stuck → `OpenestDirection`: 12 directions, rays of 3/7/12 m at 1.2 m, scored by free distance
    plus a bias toward the goal, held 2–3 s (a doorway wins over a wall 1 m away). Bots now also teach the graph
    (`[nav] LearnFromBots=1`), so once one finds the way out, routes include it. Failed routes are logged (`no route:
    no node near the bot / goal / not connected`, up to 8 per bot).
  - **Support-action bar still 0, end screen 0 points:** not solved in that round; `TraceMessages` now also logs
    per-minute message counts (`message counts, last minute`). Solved in the third round (see Offline scoring).
  - **Enemy bots not on the minimap when firing:** see the third round.
- **Eighth round, damage:** the hit log showed bots at 100 health on the first hit and taking exactly the claimed
  damage (Bot12: 100 → 72 → 41 → ~6 over bursts claiming 28 / 30 / 35); no hit claim went unapplied. The
  "soaking" is how few shotgun pellets your game registers per blast (2–4, 7–15 damage each; a rifle hit did 17).
  Field correction: `ServerSoldierSoldierDamageMessage +0x30` is the health taken (from `0xE3CBB0`: before − new,
  clamped), `+0x34` the health before, `+0x38` the raw damage (the burst lines printed `+0x30` as "after"). Bot names
  now survive their player being destroyed, and the minute summary starts with an ALL BOTS total.
- **Sixth round:** bots still a bit too accurate and quick to notice; some bots took several close shotgun blasts.
  Changes: `SkillPercent` 50 → 40, `AimSettleMs` 600 → 900, `NoticeMsPer10m` (extra reaction per 10 m beyond 10 m,
  max 1 s). New combat log (`src/combatlog.cpp`, `[combatlog]`): per bot each minute - shots
  (`ServerWeaponPlayerPrimaryFireShotSpawnedMessage`), hits and damage (`ServerSoldierSoldierDamageMessage` from
  `0xE39170`: `+0x28` victim soldier, `+0x2C` DamageGiverInfo → `[[info]] - 8` = giver player, `+0x30` health after,
  `+0x34` before), hits on you, kills and headshot kills; and every hit you land on a bot, merged per burst, with your
  hit claims (`DamageArbitrationClientDamageMessage`: `+0x28` victim id, `+0x30` giver id, `+0x50` damage; applied by
  `0xE41590` when `SyncedGameSettings.AllowClientSideDamageArbitration` (`+0x1E`) is set, after a sequence check
  against the giver's `+0x520`) against the damage the server applied - to see where shotgun pellets get lost.
- **Fifth round: crash** (WER: access violation at offset `0xA031FD` = `0xE031FD`): the explosion spotting handler
  `0xE030A0` (explosions whose `VeniceExplosionEntityData` has `SpotTypeOnExplode` / `UnspotsOnExplode`) reads the
  thrower's team right after `getGiverPlayer` (`0xBAE090`) without a null check; a bot's grenade that lands after the
  bot died has no player any more (the game destroys dead AI players). `guards.cpp` null-checks it and takes the
  function's exit `0xE0324F`. A scan of all ~100 `getGiverPlayer` callers found no other unchecked dereference (the
  two other hits are second calls guarded by a first). No dump was written to `CrashDumps`; the event log had it.
- **Fourth round:** scoring and the special-ability bar work. No spawn kills (bots killed you 4–240 s after spawning,
  2–28 m, no headshots); still "a bit too accurate" and "turn and shoot as soon as I see them". Changes: FOV 160 →
  120°, an extra `PeripheralDelayMs` (600 ms × (angle / half FOV)²) for enemies off-center, turn rate 420 → 240°/s,
  reaction 250–700 → 350–850 ms, `AimErrorCentimeters` 90 → 110, and `AimSettleMs`: up to 3× aim error right after
  acquiring a target, fading over 0.6 s. Minimap and fireteams: see their sections.
- **Third round** (3 sessions, grenades 20–40 a minute, no crash): feedback was spawn deaths to enemy snipers, the
  empty special-ability bar, and firing bots missing from the minimap.
  - **Support-action bar / 0 points:** the per-minute counts showed `UIPlayerScoreUpdatedMessage` (kills/deaths)
    but never a score event (`0x6182A896`) or `StatSupportActionPointsMessage`. Root cause and fix: see Offline
    scoring below.
  - **Spawn sniping:** bots respawn through `ServerPlayerForceRespawnMessage` → `spawnPlayer(0, player, 1, 0)`
    (`0xE3A940`) → the tactical spawn-point chooser (`*0x2AF3984`, `finalFilterVisibility` `0xEE4550` drops points a
    living enemy can see) — the same fall-back spawn a human gets. What they never did is spawn on their fireteam
    buddy, and many bots roll `Kit_Sniper`, which always aimed at the head after 0.25–0.7 s. Now: bots spawn on
    their buddy when it is out of combat (Spawning below); snipers settle `SniperSettleMs` (+0.4 s per 100 m) before
    the first shot and go for the head only about `SkillPercent` of the time; any soldier that spawned less than
    `SpawnGraceMs` ago gets the remainder as extra reaction. Each of your spawns and the kills you're part of are
    logged (`you spawned at ...: buddy BotNN 3 m away; nearest enemy BotMM 85 m; 1 enemies with a line of sight to
    you`, `kill: Bot07 (sniper) -> you (headshot), 112 m; the victim had been alive 0.8 s`).
  - **Minimap:** see Spotting below (`SpotDiagnostics` was a one-round diagnostic, since removed).
- **Not yet:** weapon switching, crouch/prone, friendly-fire checks, target pose (aims assume standing), bots using
  support actions.

## Offline scoring (`src/scoring.cpp`, `[scoring]` in the ini)
Every score event in MOHW goes through the online stats object (`*0x2B14928`, null offline):
- Kills, damage, objectives become stat triggers; the stats object measures them against the `ScoringTypeData`
  list of `mohw/persistence/scoring/scoring` (`[stats+0x64]` → `0xE7DAF0` / `0xE7D9C0`) and `0xE7B2F0` dispatches
  the **score message** (`StatEventMessage` subclass, type `0x6182A896`, category Stat `0x7C89B9B7`, ctor
  `0xE1DBF0`, 0x6C bytes): `+0x28` player, `+0x30` points (`Score + AdditionalValueMultiplier × value`), `+0x34`
  `DescriptionSid`, `+0x38` bucket, `+0x3C` visibility, `+0x40` StatEvent, `+0x50` `BuddyBonusPoints`, `+0x54`
  `BuddyBonusDescriptionSid`, `+0x64` `BuddyDistanceLimit`, `+0x68` support-action flag, `+0x69` `RequestVO`.
- Its listeners: the **scoring system** (`0xE831B0` → `0xE810C0` → `0xE7F130` → `0xE7E4E0`: round score, the
  `UINetworkHudScoringMessage` popup to the player's connection, the fireteam buddy bonus via `getFireteamBuddy`
  `0xE3A8A0`; AI players get nothing) and the **support-action collection** (`ServerSupportActionCollection`,
  listener `0x1718400` → `0x1716560`: if `+0x68` is set, `addPoints(player, +0x30)` (`0x1714E30`) and
  `addPoints(buddy, +0x50)`). The points live in the player's `ServerSupportActionSubView` (player `+0x41C` →
  `0xE1B1A0`, `+0x20`, replicated with dirty bit 1); crossing 400 / 900 / 1500 / 2300 (`0x2A2E078`) grants the next
  action (`0x168FBE0`, `0x170C3C0`).
- Offline nothing triggers, so nothing ever scored. `scoring.cpp` records `ServerPlayerKilledMessage` in the dispatch
  hook (`+0x28` victim, `+0x30` killer, `+0x55` headshot = hit body part 1, set by `0xE44040` / `0xE41A40`) and on
  the next server tick sends the score message for a human killer of an enemy: "ID_IGHUD_SCORE_KILL" 100 (buddy 10
  within 20 m) and "ID_IGHUD_SCORE_HEADSHOT" 50 (buddy 5) — the game's values for the plain kill and headshot
  entries. Multi-kills, assists, spot assists etc. are not emulated. Skipped when the stats object exists.
- The dispatch hook (`MessageManager::dispatch`) is now always installed; `TraceMessages` only switches the logging.
- **Objectives (2026.11.10.1).** "Armed/disarmed 0 charges" on the end-of-round screen after arming two in Combat Mission
  (Basilan). That number is the reflected `PlayerScore.FlagsCaptured` (record score +0x164; `RunningFlagsCaptured`
  +0x174 is the match total, kept in step by `scoreboard::Tick`), and only the scoring system's score-message handler
  `0xE7F130` adds to it (`0xE15B00`), for StatEvents 115 (capture point), 173 / 175 (Combat Mission charge armed /
  disarmed), 190 (Home Run flag captured) and 202 (CTF). The objective modes score through their tokens:
  `TokenAction_GiveXP` (`StatEvent` +8, `aliveStatus` +0xC, `TargetTeam` +0x10, `wholeTeam` +0x14, `includeSelf` +0x15,
  `ForceUseTargetTeam` +0x16; executed by `0xB72760`) dispatches `StatTokenPointsMessage` (`0xA9E33CD8`, +0x28 player,
  +0x2C StatEvent) for the stats object, which offline isn't there. From the level data:
  `combatmission_interact_token` gives 173 (attackers) / 175 (defenders), `combatmission_bombsitemanager_token` 177 to
  the planter and 178 to the rest of the team (11 messages per destroyed objective in a 10 v 10 - the per-minute counts
  showed exactly that), Hot Spot uses the same tokens, Home Run's `sport_manager` 188 / 190 / 197 / 199, `ctf_tokens`
  200-207. No `StatEventTriggerEntityData` or `ArmDisarmAwardEntityData` is placed on any map checked. Now
  `scoring.cpp` records those messages and, on the next server tick, sends your score message with the entry's
  `ScoringTypeData` values (173 / 175: 200, "ID_IGHUD_SCORE_BOMB_PLANTED" / "_DEFUSED", buddy 100 within 7 m; 177: 50;
  178: 100; Home Run 188: 100, 190: 200, 197: 200, 199: 100); bots' rows get the points (`scoreboard::CreditPoints`).
  Dispatch is synchronous, so the log line has the new count: `scoring: you: a charge armed, +200 (score event sent);
  objectives on the row 1`. Sector Control captures (115) don't come through tokens and stay unscored.

## Voice chat crash on Tab (2026.11.10.1)
Pressing Tab while dead closed the game (2026-11-07 13:26 and 2026-11-10 15:10, both the last line of the log:
`cat 0D7638B3 type 904AA3D9`). WER: access violation reading 0x48 at `0x1C29312`. The UI, as it reads the `VOIPOn`
profile option (`0x179FE90` / `0x17A2E90` / `0x17A2EE0`), queues a `VoiceSettingsMessageBase` (`0x904AA3D9`, category
Voice `0x0D7638B3`, +0x28 setting: 6 = VOIP on, 7 = off). Its handler `0xE2BA00` (from `0xE34E90`) resets the voice
manager's channels (`0x1C21080`, `0x1C20F00` -> `0x1C20380`), which hands `VoipGetRef()` (`0x1C2B120`, `[0x2B4447C]`)
to `VoipControl` (`0x1C1FE50`) for the speaker / mic masks ('uspk' through `0x1C20100`, 'umic' through `0x1C200E0`);
`VoipControl` reads the ref unchecked. Offline no voice session starts and the ref is null. `guards.cpp` hooks both
wrappers: with no ref they return -1 (VoipControl's error value; the callers ignore it). Logged once:
`voice: speaker channel change skipped - no voice session (offline)`. `imagetest` calls both with a null ref.
- The ctor can't run in `imagetest` (it builds eastl strings through `0x401DCF` / `0x40173F`, thunks inside the test
  exe's own range); the field mapping is from the disassembly.

## Spawning (`[bots] SpawnOnBuddy`, `BuddySafeMeters`)
MOHW lets a player spawn on their fireteam buddy unless the buddy is in combat (`SquadSpawnFailReason`:
`CombatSuppressed`, `CombatProjectile`, `CombatStrike`; `ControllableEntityData.CantSpawnOnBuddyCombatTime` +0x90),
else "Falling back" to a chooser spawn point. Humans choose with `NetworkSelectSpawnGroupMessage` (`PlayerToSpawnOnId`
+0x48 → extent `0xE1EAA0`). Bots now follow the rule: on (re)spawn, `getFireteamBuddy` (`0xE3A8A0`, only if the
pairing is mutual); if the buddy is alive and not in combat — a bot buddy hasn't fought within its
`CantSpawnOnBuddyCombatTime`, no enemy within `BuddySafeMeters`, none of the 4 nearest enemies within 100 m has a line
of sight to it — `spawnAt` 1.5 m behind it (else beside it) where there's no wall in between and ground underfoot;
otherwise the fall-back spawn. Logged per spawn (`spawn on fireteam buddy`, `buddy ... is in combat (...)`, `no
fireteam buddy`).

**Water (ninth / tenth round):** bots were seen in the water at the map edge. Buddy spawns skip a swimming buddy and
any candidate spot with water over it. Every spawn logs the bot's pose, state and water depth (`has soldier ... at
(...), standing, on ground[, in N m of WATER]`). Tenth round: no spawn was in water. The one swimmer (Bot17) spawned
on land, called itself stuck on its very first tick (the stuck timer started at 0 with the progress point = its spawn
point), and the detour's "most open direction" was the open sea - it walked in. The ninth round's fix for that, a
`ServerPlayerForceRespawnMessage` for a bot found swimming within 5 s of spawning, **faulted inside the engine**
(`0xE3ED37`, the spawn path, on a player whose soldier was still alive) and the game hung 40 s later: never force a
respawn of a living soldier. Removed (`RespawnFromWater` is gone). Instead:
- **Water depth probe** (`eng::WaterDepth`): a ray from 2 m above a point to 4 m below it, once counting water and
  once not (`kRay_DontCheckWater`); the difference between the two hit fractions (`RayCastHit +0x30`, `eng::RayHit`;
  `+0x00` is the hit position, `+0x10` the normal) × 6 m is the depth. Swimmers log the probe's reading, to confirm
  it sees the sea.
- **Obstacle avoidance and detours avoid water:** the 0.15 s probe ahead counts water 2 m ahead (off a route) as
  blocked, the fan-out skips wet directions, and the most-open-direction pick scores open water -100.
- **No stuck call on the first tick** (the timer starts at 1.5 s).
- **Swimmers head for dry land:** the last spot they stood with no water over it (checked once a second), else the
  nearest dry nav node within 40 m.
- **The nav graph stays dry:** nothing is learned while a walker swims or slides (the chain restarts on foot), and
  every node is probed in the background (2 per tick, a full pass in ~10 s); nodes with water over 0.4 m deep are
  marked wet - no route, nearest-node lookup or new link uses them (`nav: N node(s) found with water over them`).

**Fall-back spawns in the bot's own base (fifteenth round, `[bots] FallBackToOwnBase`).** Bots spawned in the other
team's base. MP_03 TDM's 49 `AlternateSpawnEntityData` points (10 team 1, 13 team 2, 25 shared) are all open to
anyone: the tactical chooser (`g_SpawnPointChooser` `*0x2AF3984` = the entity +0x20; pick `0xEE9FF0` → `0xEE8690` for
TDM) gathers them (`0x4CB420`, TypeInfo `0x2B77ECC`) and filters them in `0xEE1C90`: `Enabled`, then - only when
`TacticalSpawnPointChooserEntityData.TeamFilter` (+0x160) is set, which it isn't here - drops a point whose `Team` is
neither 0 nor the player's. Both values are property reads (thiscall(out, context), ret 8, out = {override, &field};
the value is `*(*override & ~3)` when `*override` has bit 0 set, else `*field`, `0x564260`): `0x26BC090` (TeamFilter)
and `0x25D0770` (point Team). While a bot's fall-back spawn runs (on the server thread), the hooks make TeamFilter
read true and a shared point's team read 99, so only the bot's own team's points survive. No free point there (the
chooser fails): the game's own pick (the second `ForceRespawn` only runs when the first left no soldier). Logged per
spawn: `fall-back spawn from its own team's points only (the chooser looked at team N's 10, shared 25, the other
team's 13)`. Your own spawns are untouched.

**Shared spawns when a base is camped (`[bots] SharedSpawnWhenCamped`).** Own-base-only fall-backs left a pushed team nowhere else to come in. Now
each team's base is the average of where its own-base spawns landed, and it counts as camped when `CampedEnemies` (2) living enemies are within 30 m of
that, or `CampedSpawnDeaths` (3) of the team were killed there within 15 s of spawning in the last 30 s. It stays camped until 25 s after the last time
either was true (checked at each spawn). Meanwhile the point-team hook flips: a shared point keeps team 0 and the bot's own points read as nobody's, so the
chooser keeps only the shared ones, and its own visibility test still applies. Early in a round the levels offer 10 / 0 / 10 points (no shared ones), so the
bot falls back to its base. Logged: `spawns: team N's base is camped`, `its base is camped: shared spawn at ..., nearest enemy N m`, `base is clear again`.
Your own spawns are still untouched.

Numbers it was set from, seven rounds from `r29` on: bases 23-47 m across at most (one outlier 68 m). Deaths within 15 s of an own-base spawn per team per
round: Somalia 0-6, Novi Grad 4-22, Sarajevo 7-15. "3 in 30 s" would have fired 0 times on Somalia and 0-4 times a round on the other two, "2 in 30 s" up to 8.

**First round with it (Basilan, build .2): too eager, and the shared points weren't safe.** "2 enemies near at the moment of a spawn" fired at minute 4 for both teams and never cleared: 134 shared spawns against 64 from the bases (bases 77 m apart). Deaths within 15 s of spawning: shared 32% (median life 22 s), base 6% (56 s). 38 of 134 landed under 10 m from an enemy, 88 under 20 m, and 32 of team 2's 64 landed within 30 m of the base they were leaving. So now, from build .4:
- Enemies must stay: `UpdateBases` samples every tick, seconds with `CampedEnemies` near add up and fall back when they aren't, and `CampedSeconds` (10) makes it camped. The hold is 15 s.
- The point-team hook also hides a shared point within `SharedSpawnSafeMeters` (25) of an enemy or within 30 m of the bot's own base. No safe point: its own base. It reads each point's position at data +0x40 (`SpatialEntityData.Transform` at +0x10, its position 0x30 in; `Team` at +0x60 comes after that class). That assumes the stored position is a world position, unproven; each shared spawn logs how far it landed from the nearest stored position, and 3 landings over 5 m turn the skipping off.

## Posture and character state (ninth round)
Reported: bots lying prone and "sliding" about, shooting off to the side of where their body pointed, and taking hits
without dying. Bots never press crouch, prone or change-pose, so the DLL now reads what the soldier is actually doing:
- **Pose:** `soldier+0x1A4` → `CharacterPhysicsEntity`; `+0x80` current pose, `+0x84` the pose it's changing to
  (`CharacterPoseType` 0 stand, 1 crouch, 2 prone; `setPose` `0x49A3E0`, getters `0x48B6E0` / `0x48B6F0`). The
  input pose handler (`0x7EA9A0`) changes pose only on crouch (17) / prone (16); **jump while prone stands the soldier
  up instead of jumping** (`0x7EA930`), and sprinting stands a crouched one up (`0x7F2030`).
- **State:** `[physics+0x64]+0x10` = `CharacterStateType` (`0x1A15370`): 0 on ground, 1 jumping, 2 in air, 3
  climbing, 4 falling, 5 parachute, **6 swimming**, 7 animation-controlled (vaults), **8 sliding** (slope too steep),
  9 sprint-sliding, 10 rappeling. A swimming soldier lies flat - the likeliest "prone and sliding" bots, since they
  were also seen spawning in water.
- The brain logs `found itself prone at (...) (on ground); standing up` and presses jump (a 0.12 s press 0.3 s in,
  then every 0.8 s, also mid-fight) until it stands; `swimming at (...)` heads back to dry ground (no fighting while
  swimming). Tenth round: no bot was found prone or swimming except the one above; ~40 "found itself crouched
  (animation-controlled)" lines were vaults (special moves crouch on their own), so only a crouch/prone **on the
  ground** counts now.
- **Aim by pose:** eye, chest and head heights now follow the pose (standing 1.6 / 1.1 / 1.65 m, crouched 1.05 / 0.75
  / 1.1, prone 0.4 / 0.25 / 0.35). Aiming at a standing chest over a crouched or prone soldier went over its head.

## Damage between bots (ninth / tenth round)
The combat log showed bot hits apply correctly (100 HP, the claimed damage, death at <= 0, the player destroyed) - but
bots fired ~400-600 shots a minute each and hit with 1-4% of them (all bots: 3782 shots, 62 hits in one minute).
That's the "bullets hitting around bots that don't go down": near misses (3658 `ServerSoldierSuppressionSuppress`
messages that minute). The aim error was tuned for fights against you. `[brain] BotTargetAimErrorPercent=60` (default)
tightens it against other bots only, plus the pose-aware aim above.

Tenth round: still 4,000-6,000 bot shots a minute for ~13 bot kills (0.4-3.8% hits; single bots 500-600 shots and 0
hits in a minute). The aim pitch convention was checked against the game's own aim builder (`0x802190`: yaw =
2π − atan2(forward.x, forward.z), pitch = atan2(forward.y, up.y): positive up, as the bots use) - not the cause. What
is: bots fired from the hip, on the move, in long full-auto bursts, plus the simulated recoil climb (up to ~2° after
half a second: over the head at 20 m). Hip fire's spread is several times ADS spread, and it blooms during a burst.
Now:
- **Aim down sights** while engaged (`EIAZoom` 13 held; the zoom handler `0x82B9B0` toggles only when the input's
  `+0x3C0` bit `0x10` or the weapon data's own flag is set - hold mode for the bots), except within 6 m and while
  lining up a grenade; snipers always scope in.
- **Shorter bursts:** 0.3-0.5 s within 15 m, 0.2-0.35 s at 15-40 m, 0.1-0.2 s taps beyond; support 0.4-0.8 s.
- **Strafe only within 30 m** (and the move penalty only applies then); beyond 50 m they close in.
- `RecoilPercent` default 100 -> 40.
These also make bots more accurate against you; `AimErrorCentimeters` / `SkillPercent` are the knobs if they're now
too sharp.

Eleventh round: no better - 2-4.5% of bot shots hit (e.g. 3,933 shots, 98 hits in a minute), ~10 bot kills a
minute. Your deaths: 4 by bots, 3 of them headshots (15, 6 and 10 m) - why "a couple of shots" killed you. The log
couldn't tell bad aim from hits the server never registers, so bot bullets are now traced:
- `ServerCollisionProjectileFireMessage` (category `0xDB60E01E`, type `0xAF088758`, sent by `0xBC63CE` per projectile)
  carries the projectile (+0x28) and its spawn point (+0x30); the projectile's velocity is at +0x120. The shooter is
  the actor whose chest is nearest the spawn point; its aim (the brain's yaw / pitch) and target come from bots.cpp.
- Per minute, separately for bots shooting bots and bots shooting you: bullets traced; the **spread** between the
  bot's aim and the bullet's flight (the weapon's own dispersion - small if aiming down sights works) and its average
  offset; how many paths **crossed the target's body** (a vertical segment for its pose, 0.3 m thick; head / torso /
  legs bands); how many of those the server **registered as hits**; misses over / under / beside and the median miss
  distance; hits with no traced crossing (explosions, bystanders).
- Every hit on you: `combat: BotNN hit you for D (health A -> B) at N m, head|torso|legs` (the band its bullet crossed).
Many crossings with few registered hits would mean bot soldiers' server-side hit boxes aren't where their bodies are;
few crossings would mean aim or spread.

Twelfth round - the answer: the bullets fly where the bots aim (spread median 0-0.9 deg, ADS works) and 55-82% of
bot bullets at bots cross the target's body, but **only 1-9% of those crossings register**. Bot bullets at you: 15-38%
of crossings register (the 0.3 m cylinder is generous; real capsules are thinner). Bot bullets pass through bots.
Why, as far as the code shows:
- Server-side soldier hit shapes are `ServerBoneCollisionComponent`s (vtable `0x27BB3B8`; per-bone capsules from
  `SkeletonCollisionData.BoneCollisionData`, `BoneCollisionData` 0xB0 bytes with `ValidInLowLod` +0xA0 /
  `ValidInHiLod` +0xA3). Their frame callback (`0xB7D9E0`) schedules an update (+0x210) when the history interval
  has passed (`0x7ED7B0`); the update (`0xB73AA0` -> `0x7FCC10`) places every capsule, records the lag-compensation
  history and marks the shapes valid (+0x1CD). The lookup (`0x7F52E0`) refuses invalid shapes and, per bone, needs
  `ValidInHiLod` or `ValidInLowLod` for the LOD flag (+0x1C0).
- The component only runs while the server has an animation manager (`0xB6E3B0`: ServerGameContext +0x18). Server
  startup (`0xAA97F0`) creates it (0x970 bytes, ctor `0x68A7F0`) **only for game type "multiplayer"**: the server
  setup (`server +0x60`) +0x73 clear = "singleplayer", +0x74 = "coop" (`0x7B90E0` / `0x7B9100`, the same flags
  name the GameType at `0xAA9B0B`), then `GameAnimationSettings.ServerEnable` (default 1; the class defaults are
  in `0x25C0BA0`). An offline match started through `-webmode spoffline` may well be "singleplayer".
- `[bots] ForceServerAnimation=1` (default) NOPs both game-type jumps (`ServerAnim_skipIfSinglePlayer` `0xAA9FCE`,
  `ServerAnim_skipIfCoop` `0xAA9FDD`; the image smoketest checks the bytes). The log now says per level
  `server: game type ...; server animation manager ...`, and the bullet trace splits crossings and hits by whether
  the target was near you / in your view (your game animates what it can see) or elsewhere.

Thirteenth round: the server runs the offline match as **"coop"**, and even with both game-type checks NOPed **no
server animation manager was created**. One of the two remaining conditions blocks it (the server setup's +0xA8 list is
empty, or `ServerEnable` is 0); the log now prints both (`server: setup +0xA8 list ..., N entries; ...ServerEnable`).
Registration stayed at 0-6% of crossings, **also for bots near you or in your view**, so it isn't a matter of what
your game animates. Re-reading the soldier damage entry (`0xB64FE0`) corrected an old note: a hit flagged
client-arbitrable (damageInfo +0x6D bit 8) with `AllowClientSideDamageArbitration` on is **dropped when the shooter is
AI** (+0x354) or +0x424 and applied (soldier +0x6E8 handler) for a human; unflagged hits are always applied. The
entry is now hooked (`ServerSoldier_onDamage`): per minute, how many hits reached a soldier bot -> bot / bot -> you /
you -> bot and how many were flagged, plus three raw 0x80-byte damage records each for bot -> bot and bot -> you.
That says whether bot bullets never reach bot soldiers (collision) or reach them and get dropped (arbitration), and
the records are a template if the fix has to apply bot-vs-bot damage itself.

**Fourteenth round - found and fixed.** The server's setup +0xA8 list is empty (0 entries) with `ServerEnable` 1, so
no server animation manager in "coop" whatever the patch does - and it doesn't matter: the damage entry counts show
**bot bullets do reach bot soldiers** (400-1,900 hits a minute, 50-80% of the body crossings) and **every one is
flagged client-arbitrable**, which the entry drops for an AI shooter. In "coop" the server leaves AI hits to the
players' games (a client reports the hits it sees, `DamageArbitrationClientDamageMessage`, giver = the bot) - the
~40 bot-vs-bot hits a minute that counted were the ones your game saw. Bot hits on you work the same way (server: 63
hits in a minute, all dropped; your game reported 25).
`[bots] BotHitsOnBotsByServer=1` (default): the damage-entry hook applies a flagged hit whose shooter is AI and whose
victim is a bot itself, the way the entry applies an unflagged hit (`soldier +0x6E8` handler, vfunc 0 (damageInfo,
giverInfo)); the claim listener (`0xE41590`, `ArbitrationClaim_onMessage`, ids through ServerPlayerManager +0xA4) is
hooked to ignore claims where a bot hit a bot, so nothing counts twice. Bot hits on you are left as they were (your
game reports them) - the server sees 2-3x more of those than your game reports, so making it authoritative there too
would make bots much deadlier against you. `BotTargetAimErrorPercent` goes back to 100. The per-minute damage line
adds `bot -> bot hits applied by the server N, players' reports of bot -> bot hits ignored M`.

Fifteenth round: it works. 89-192 bot -> bot hits a minute applied by the server, no faults, 18-28 bot kills a
minute (was ~10); the applied count exceeds the damage-entry count by about the kills (a killing hit's victim is gone
from the actor list by the time the minute is counted). Bot fights now end in seconds. You: 4 deaths in ~6 minutes,
alive 48-113 s each; bots hit you 4-6 times a minute (18-44% of their body crossings).

## Grouping (fifteenth round)
Both teams moved as one pack: with no target, every bot headed for the **nearest** enemy, so teammates spawned
together walked the same route. Now each bot rolls a leaning once (hunter, flanker, roamer) that sets its odds of
three goals when it picks one: **hunt** one of the three nearest enemies (the nearest 55%, then 30 / 15%); **flank**
- a learned nav node 12-28 m to one side of the line to an enemy, 50-80% of the way there (not within 15 m); **roam**
- a random nav node 25-140 m away, the one of four candidates farthest from its teammates. With two or more teammates
within 8 m the hunt odds drop to 40% and roaming's rise 1.5x. Hunt goals last 6-10 s, flank / roam 12-18 s.
Logged once a minute: `spread (last minute): a bot had N teammates within 8 m on average; most bots in packs of 3+
P% of the time; goals with no target: hunt H, flank F, roam R`.
Sixteenth round: 0.2-0.6 teammates within 8 m on average, packs of 3+ 0-8% of the time (8% only in the first minute,
when everyone leaves the bases together); goals 45% hunt, 28% flank, 28% roam.

## Demolitions bots throw mini grenades (fifteenth round, `[bots] DemoMiniGrenades`)
Bots' remote charges never went off. The Demolitions kit's throwable explosive (`CA_THROWABLE_EXPLOSIVES`, WeaponTable
slot 6) goes out with the grenade input, but it's set off by `CA_THROWABLE_EXPLOSIVES_DETONATOR` (slot 3) =
`C4Remote` (`MOHWDetonatorWeaponData`, fired with `EIAFire` once selected), which bots never switch to. Instead of
setting charges off for them, Demolitions bots now carry the Support kit's `W_GR_V40_MiniFrag`, which is also slot 6
(the grenade slot; the slot index is the weapon slot). The game's default loadout (`0xB41B70`, no weapons in the
message) takes the first `SoldierWeaponUnlockAsset` of each WeaponTable part and selects it for that slot
(`0xB41A10`, slot = part index), so while it runs for a Demolitions bot the kit's parts 6 and 3 point at the Support
kit's (3 is empty there: no detonator); they're put back right after. Logged per bot:
`[NAME] Demolitions: W_GR_V40_MiniFrag (slot 6) instead of remote charges, no detonator`. Sixteenth round: all 15
Demolitions spawns logged the swap; 2 explosion packs were placed in 8 minutes (none attributed to bots).
Kept for reference (an earlier version detonated bots' charges itself): `ServerCollisionExplosionPackPlacedMessage`
(category `0xDB60E01E`, type `0xD6E2E3A6`, +0x28 the pack, sent by `0xBD5190` as it starts out); `...DestroyedMessage`
`0xF90DA03A`; `ServerExplosionPackEntity` +0x14 data (`ExplosionPackEntityData` +0x17D `HasRemoteDetonator`), +0xBC
DamageGiverInfo (owner), +0x120 velocity, +0x210 state; `setState(4)` (`0xBC2270`) detonates - the remote detonator
(`0x1695030`, the loop in `0xEADA00`), timeouts and proximity triggers all pass 4.

## Names (fifteenth round, `[bots] Names`)
Bots take a random unused name from the list (Dingus, Ol Red, Slick, ...; 19 by default) and keep it through
respawns; "Bot07"-style numbers once all are in use. Sixteenth round: names in use on the server, but none on your
scoreboard - bots had no rows at all (Scoreboard below).

## Bots keep their player (sixteenth round, `[bots] KeepPlayers`)
Reported: bots that died, or spawned on their fireteam buddy, showed up limp (ragdoll pose) and slid about with
animations. MOHW deletes an AI player as soon as its soldier dies or unspawns - the campaign's rule for enemy soldiers;
a human keeps theirs and respawns. Three soldier paths (`0xB61BC0`, `0xB6BE10`, `0xB18AD0`) unlink the soldier
(`ServerPlayer::setSoldier(null)` `0xB3C720`, which also clears the body's player link through the soldier's
`setPlayer(null)` `0xB68200`), then `cmp byte [player+0x354], 0; je skip` -> release its loadout (`0xB42D40` ->
`0xB417C0`) and `ServerPlayerManager::deletePlayer` (`0xB3AC90`, which sends `ServerPlayerDestroyMessage`). So every bot
death made a new player: a new id (ids are reused), no scoreboard history, and your game deleted the ClientPlayer the
falling body belonged to - the likeliest cause of the limp, sliding bodies (the server's spawns were normal: standing,
on the ground). Now each site's 9 bytes are `call stub; je skip; nop; nop`; the stub pushes the player's register
(edi / edi / esi), asks `ShouldDeleteAiPlayer` (AI and not one of our bots) and sets ZF from the answer, keeping ecx /
edx. A dead bot keeps its player, name, team, kit, fireteam and row; after `AutoRespawnSeconds` it respawns through the
same buddy / fall-back spawn with a fresh brain (same leaning). Logged: `bot NAME: died; respawning in 5 s (same
player)`, `bot NAME: respawn`. The kit stays for the whole match: the default loadout (`0xB41B70`) replaces the
appearance unlocks but sets weapons per WeaponTable slot only where the kit has one, so a new kit could keep the old
one's weapon in an empty slot. `imagetest` runs a patched site's stub: a human is skipped (ZF 1), another AI player is
deleted (ZF 0), ecx / edx unchanged.

## Scoreboard (sixteenth round, `[bots] Scoreboard`, `src/scoreboard.cpp`)
Bots had no names or numbers on your scoreboard. Its rows are the scoring system's per-player records: the scoring
system (`[listener+4]`; its ServerPlayer listener has vtable `0x281D8B4`, built by `0xE62AC0`, handler `0xE80F40`)
keeps an eastl::vector of records by player id at +8; a record (0x49C bytes, ctor `0xE7BDA0`) holds at +0xA8 the
replicated `PlayerScore` (0x178 bytes, ctor `0xE23030`; `SvPlayerScore` / `PlayerScoreGhost` / `ClPlayerScore`):
+0x140 kills (`0xE15A80` +1, dirty 2), +0x144 deaths (`0xE15AA0`, dirty 4), +0x148 points (`0xE15C00`, dirty 8), +0x154
float, +0x164 (`0xE15B00`). The handler makes a record (`0xE7C710`) only on `ServerPlayerCreateMessage` for a player
with +0x354 (AI) and +0x355 (spectator) clear, and drops it (`0xE7CAE0`, `0xE7A650`) on `ServerPlayerDestroyMessage`
for the same; bots never even send the create message. Kills count only for a killer that isn't AI (`0xE7A6B0` ->
`0xE77180`), round score only for a player with +0xD0 set and a connection (`0xE7E4E0`); deaths (`0xE7E480` ->
`0xE7C800`) have no AI check. Your game: `ClientGameContext +0x44`, list at +4 (node +8 = the entry); entry +0x158 weak
reference to the ClientPlayer, +0x164 / +0x168 / +0x16C kills / deaths / points (the server fields + 0x24; the row
builder `0x16EF6A0`, `kd` from `0x16EF3C0`, `points`). The row's name (`0x16EF570`): an AI player's name string
`[player+0x10]` (set by `setPlayerName` `0xB3EB20`, replicated with the player's dirty bit 2), else the inline name at
+0x28. Now: the handler is hooked (7 bytes); a bot's `ServerPlayerSwitchTeamMessage` (its setTeam) makes its record
just before the game's own team update for it, and an AI player's `ServerPlayerDestroyMessage` drops its record. A bot
killing an enemy gets kills + 1 and `KillPoints` (+ `HeadshotPoints`) on its points (`scoreboard::CreditKill`); deaths
count by the game. Logged once a minute for ten minutes, server side `scoreboard (server, kills/deaths/points): N rows,
...; records added A, removed R, failed F` and your game's list `scoreboard (your game, kills/deaths/points): N rows:
NAME (AI) 3/2/300, ...`.

**Seventeenth round:** rows for all 11 bots (records added 11, failed 0), names on your scoreboard, server and your
game agreeing on kills / deaths / points every minute. Reported still missing: bot names on the "killed by" card and
any scores on the end-of-match best-fireteam scene. Found while looking: **your own points were 0** all match (15
kills). Fixes:
- **Points (yours too).** A row's points are `+0x148` = the sum of the RoundScore buckets (the six class buckets,
  `sc_light_assault` ... `sc_support_gunner`) minus penalties (record +0, `0xE67190`). A score event lands in bucket 0
  (`sc_general`, `ScoringTypeData.Bucket` of the kill entries; not a RoundScore bucket) and then in the player's class
  bucket through the kit's unlock category (`0xE15EA0` -> `0xD41F60` -> `0xE6B010`) - which offline never resolves, so
  the recompute after every score event set 0. Now the DLL keeps a tally per player (bots and you: `KillPoints`,
  + `HeadshotPoints`) and re-applies it 4 times a second (`scoreboard::Tick`).
- **Match totals.** The end-of-round screens - the results list (`0x17C5E90`, `score` = entry +0x194) and the best
  fireteam scene (`0x16F8080`, `0x16F7770` with its flag) - show the PlayerScore totals +0x168 kills / +0x16C deaths /
  +0x170 points / +0x174 flags (dirty 0x400; your game's +0x18C..+0x198), which the game adds up (`0xE15D70`, cleared
  by `0xE15DB0`) only on `ServerRoundSubRoundOverMessage` (`0x42983801`, handler `0xE81020` -> `0xE66F20`). TDM never
  sends one (the log has only `ServerRoundOverMessage`), so they were 0 for everyone. Now totals = the round's numbers,
  kept in step by `scoreboard::Tick` until a sub-round end shows up (then they are cleared and the game takes over).
- **"Killed by" name.** The card (`0x17D9380`, `title` = card +0x11C) gets the killer's name from `0x17D4FA0`: the
  online name at Player +0x28 - the first of four online ids `{u64 id; char name[24]}` at +0x20/+0x40/+0x60/+0x80
  (Player ctor `0x8134B0`) - with no AI check; an AI player's name is the string at +0x10 (the scoreboard's
  `0x16EF570` picks that one for AI). Your game's AI players now get that name copied into +0x28 once a second
  (`scoreboard::ClientTick`, client thread).
Logged: the minute lines now add the match total points in brackets: `NAME kills/deaths/points (total)`.

## Limp bots that keep fighting (seventeenth round)
Still seen with players kept: bots moving about after dying (or respawning) with their body stuck in a ragdoll pose.
The log shows your game ragdolling a bot (`JuiceSoldierRagdollActivateMessage` + `ClientPlayerManDownMessage`) 0.5 s
before the server's first kill, right after one of your game's hit claims: your game applies its own claims at once,
and since the fourteenth round the server ignores its bot -> bot claims and applies its own bot -> bot hits (which
register on 2/3 of body crossings - not always the same bullets). A claim the server never matched can kill a bot on
your screen only; it goes limp there and fights on on the server. Ragdolls still equal kills per round (133 / 133):
the bot's real death later doesn't ragdoll it again. Now (`combatlog.cpp`): server-applied bot -> bot hits and
ignored claims are matched (same soldier and shooter within 0.3 s, either order); an unmatched claim is your game's
own damage (counted for 3 s), and a claim that - with that damage - reaches the bot's health on the server (soldier
vfunc +0xD8, `0xAD4070`) is applied through the game's claim handler with its damage (+0x50) raised to health + 100:
the bot dies on both sides. Logged per minute: `players' reports of bot -> bot hits ignored N (M never matched by a
server hit), applied as killing blows K (your game showed the bot dead)`.

## Round over (seventeenth round)
Bots fought and respawned for 30 s behind the results screen (`ServerRoundOverMessage` `0x16BABB47`, category
`0xD93DD7C2`, at 11:24 of the round). Now they stand still with a neutral input and don't respawn until
`ServerRoundResetMessage` (`0x4BB8DE5C`) or a new level: `round over: bots stand down ...`.

## Round length (eighteenth round, `[launch] GameModeCounter`)
MP_05 10v10: the round ended after 5:07 - Team Deathmatch (`MOHW/Gameplay/GameModes/TeamDeathmatch`) is two
`KillCounterEntityData` (MaxKillCount 75, TeamId 1 / 2, IgnoreAI false, ReinforceAmount 20) and a round timer
(`.../PF_RoundTimer`: `MPRoundTimerEntityData.RoundTime` 600 s), and 20 players make ~28 kills a minute.
`ServerKillCounterEntity` (`0xEF1DD0`, vtable `0x2824520`) - and the ticket counters (`ServerTicketCounterEntity`
`0xED4BC0`), and the client's kill counter (`0xDECAC0`) - multiply the target by `SyncedBFSettings
.GameModeCounterModifier` (+0x0C, float, clamped 1..65535) as they start; the server sends it to clients in
`NetworkSettingsMessage` (+0x54; built by `0xE40880`, applied by `0xE2D6A0`). `GameModeCounter=200` (percent, as BF3's
`vars.gameModeCounter`) sets it to 2.0 with the other settings overrides, before the level loads: 150 kills, so the
10-minute timer usually ends a round. Logged: `overrides: SyncedBFSettings.GameModeCounterModifier 1.00 -> 2.00`.

## Sliding "ragdoll" bots = bots stuck swimming (nineteenth round)
Reported again on MP_03 (not on MP_05). Ragdolls still matched server kills (290 / 289), so not phantom deaths. What
lined up with every report: bots swimming in the MP_03 bay (x 38..48, z -50..-60, water surface ~61.4) for up to
2 minutes - a swimming soldier lies flat and drifts with its swim animation. Rounds 16 / 17 / 19 (MP_03): 1 / 17 / 7
swim episodes, 3 / 65 / 16 failed exits; round 18 (MP_05, no reports): none. How they got in: "stuck at (40.1, 63.4,
-58.1)" on a ledge above the bay, then the stuck detour - a jump plus the most open direction, whose water check only
looked at the far end of the free length (12 m: land across the bay) - took them over the edge; and every exit they
tried (where they last stood dry, the nearest dry nodes) was the 2-6 m ledge. Now: the most-open-direction pick
counts water at 2.5 m and at every length it reaches; the stuck detour doesn't jump when water is within 3 m that
way; swim exits prefer a bank within 1.2 m of the water (dry node within 80 m, `nav::NearestNodeAvoiding` maxRise)
before any dry node. MP_03's learned graph has no dry low bank near the bay (its 61.4 m nodes are in the water and
probe wet), so keeping them out is what counts there.

## Stuck under a ledge (twentieth round)
MP_05: Doug stood jumping at (-57.1, 78.1..78.9, -2.8..-3.7) from 19:29 to the end of the round (5½ minutes, his
kills frozen at 4; the log only showed his first 20 "stuck" lines), Ooga Booga twice for ~27 s at the same spot; the
user spawned on Doug there three times. The spot is a nook at the foot of a learned climb: node 234 (y 78.42) → node
226 (y 80.24, 0.3 m over - a mantle someone did). Three faults fed each other: (1) the route's start was the nearest
node within 2 m of height - mid-hop that was 226, on the ledge overhead; (2) a hop (or the 2D "past it" test) counted
as reaching 226, so the bot walked at the next ledge node and that link got the blame (all of 226's and 234's
outgoing links were at the 64 maximum; the climb itself at 1.0); (3) on a route the stuck handling only penalises and
re-plans - each new route walks straight at its next waypoint, from the nook straight into the wall, and never
detours. Now: start and goal nodes rank by distance + 2 × height difference (navtest: "a route starts on the bot's
own floor"); FollowPath only advances on the ground and never past a waypoint more than 1.4 m above the feet; after
three stuck checks in a row (4.5 s) the bot leaves the route for 4 s and detours (`brain X: routes aren't freeing
it; leaving the route for a while`); the stuck line says for how long (`stuck at (...) for 4.5 s`). The six wrongly
maxed links in `MP_05_TeamDeathMatch.nav` were reset to 1.0 (backup `.before-round20.bak`).

## Stuck spots and the water probe (twenty-first round)
Two rounds on the twentieth-round build. **MP_03** (21:36, user left after 6½ min): one swim episode in place of 7-17,
no failed exits - Barttle Doo got out over the low bank at x ~53-55 in ~18 s. He still got in: stuck at (37.6, 65.6,
-59.0), 4.2 m over the bay, where `WaterDepth` (2 m up to 4 m down) never reached the water, so every probe read dry
and the detour walked him down the slope to (40.6, 63.3, -57.8); stuck at the edge, the jump tipped him in. Now the
probe reaches 8 m down (a spot on solid ground still reads 0: the ground is hit before any water under it), and the
stuck jump is off with water within 2 m in any direction. **MP_05** (10-minute round, the timer ended it): the
Novi Grad nook again - Sumting Wong ≥ 25 s at (-57.1, 78.4, -3.7) (still 20/7: he fought from it), others 4.5 s+ 
there; leaving the route didn't free him (my round-twenty reset of 234's out-links had made 234 a through-node
again). Now the stuck handling escalates: 1st check the most open way, 2nd back where the bot was 1.5-3 s before
(`freePos`), then open ways other than the one that just failed (-8 within 45°); it jumps on odd checks only. At
9 s, `nav::MarkTrap` gives every link into the nodes within 1.5 m the maximum penalty (`brain X: stuck 9 s at ...:
marked as a trap`; navtest: "a marked trap drops out of routes"). The four links into node 234 were marked by hand
(backup `.before-round21.bak`). Log: every check of a spell's first 4.5 s, then every 6 s, with pose and state
(`stuck at (...) for 6.0 s (standing, on ground); heading another way`), and `free again after N s stuck at ...`.
`ServerPlayerInstantSuicideMessage` (ctor `0x259F2E0`, 0x38 bytes, +0x28 player, +0x30 kind) is only the score
notice sent by `0xE3CE00` after a death, and its handler (`0xB23D80`) skips AI players - see the next section.

## Changing class to get out (twenty-second round, `[brain] StuckChangeClassSeconds`)
MP_03 on the twenty-first-round build (12-minute round, user 25/8): 181 stuck checks, the longest spell 6 s, every
spell of 4.5 s+ ended `free again` (7), no 9 s spell so no trap; one swim (Wang Chung, (19.9, 61.2, -63.7), not the
bay ledge); bots spread (0% in packs of 3+). 22 stuck checks were mid-vault ("crouched, animation-controlled").
The user's idea for a bot that stays stuck: change class, which kills a player so they respawn. The deploy screen's
`NetworkSuicideMessage` (`0xAAEB6754`, 0x38 bytes, sent by `0xE8CEC0`) reaches the server's network handler
`0xB42590` (the one that also takes `NetworkSelectedKitMessage`-family kit messages, e.g. `0xF7531939` → `0xB41B70`):
player ← connection (`0x628520`, `0xAF5F20`), then `[player+0x430]->vfunc[0x17C](0, 1)`.
`ServerSoldierEntity` (vtable `0x27BA820`) +0x17C is **`kill(viaEntry, suicide)` = `0xB5E790`**: a hit worth the
soldier's health (handler `+0x6E8` vfunc +0x88 + its health, damage type 0x33, giver = its player `+0x10C`) through
the `+0x6E8` damage handler (vfunc 0, or 2 when viaEntry), the suicide flag set from the second argument.
`eng::KillSoldier` calls it through the soldier's own vtable only when that slot is `0xB5E790` (signature-checked;
imagetest checks the handler's call and the slot). A bot stuck `StuckChangeClassSeconds` (15; 10 checks) in a row
is killed that way and respawns as itself 5 s later (KeepPlayers); by then the spot is already a trap (9 s). Logged:
`brain X: stuck 15.0 s at (...): changing class to get out (dies, respawns as itself)`.

## A hole that never counted as stuck (twenty-third round)
MP_05: Drumstick spawned on the user at (-48.8, 81.4, -25.9), ended up 3.4 m lower at (-51.0, 78.0, -25.0) (nav
link 195 → 772: a 4.4 m drop) and stayed there the rest of the round (~6 min, kills frozen at 4); the user spawned on
him at (-48.4, 78.4, -28.0) and changed class 10 s later (`kill: you -> you`). He could walk about inside, so the
third escape always moved him ≥ 0.6 m - "free again after 4.5 s" every ~10 s - and the count restarted: no trap (9 s)
and no class change (15 s) ever came; his log lines ran out at the 60 cap. Now a spell lasts until the bot is 5 m
(or 2 m of height) from where it began: checks that find it moving inside that don't end it (and don't count as
stuck), so the stuck checks add up - Drumstick's pattern would have been marked at ~25 s and changed class at ~35 s.
Log lines show the spell's length and how many checks found it stuck (`stuck at (...) for 12.0 s (stuck 6 times;
...)`). Bots don't spawn on a buddy bot that's stuck (`buddy X is stuck; fall-back spawn`). The pit's nodes
(772, 773, 775, 776) were marked as a trap by hand (5 links, backup `.before-round23.bak`).

## Falling into the water marks the spot (twenty-fourth round)
MP_03 on the twenty-third-round build (12½ min, user 19/10): spells now add up - 10 ended `free again` after 7.5-22.5 s,
Ooga Booga's at (19.8, 65.6, 9.0) was marked as a trap (14 links), no spell reached the class change (10 stuck
checks), 4 bot spawns skipped a stuck buddy. Five swim episodes, all short (out over the x ~53 bank in ~6 s), but three
bots (Cornwallas, Crackers, Billy) went in at the same place: stuck at (37.6-37.9, 65.6, -59.0) on the slope over the
bay, then at the water's edge (40.6, 63.3, -57.8) - Crackers without jumping - and in. The learned graph had a route
down that slope along a strip 2 m over the water: 1066 (37.7, 66.6) → 1410 (38.3, 65.4) → 1412 (40.5, 63.3, -57.9) →
1416 (41.7, 63.5, -59.8) → 1418 → 1433. Now a bot that drops into water from 1 m+ higher (its last dry spot, within 8 m)
marks the route nodes on the edge right above where it went in (nodes 0.5-3.5 m above the entry, within 1.5 m) - else
around its last dry spot - as a trap (`brain X: fell into the water at (...) from (...): marked as a trap`). 1412 and
1416 were marked by hand (6 links, backup `.before-round24.bak`).

## Third map, revive and flashbangs (twenty-fifth round)
MP_05 on the twenty-fourth-round build: clean (10-minute round, user 29/10, no swims, no traps or class changes
needed, longest spell 12 s, 3 bot spawns skipped a stuck buddy). User reports: enemy bots aren't affected by
flashbangs; a bot teammate can't be revived; end-of-round awards / leaderboards still empty; wants another map.
- **Third map:** the install has MP_03, 05, 10, 12, 13, 16, 18, 19 (`Data\Win32\MOHW\Levels\MP`; `fb2_sbtoc.py
  bundles` shows an `mp_XX_tdm` bundle). `Map=MP_05,MP_03,MP_10`; `NextMap=2` set by hand so MP_10 (Sarajevo Stadium,
  no nav graph yet) loads next.
- **Revive:** every death goes through man down (round: `ServerSoldierManDownMessage` 292, `ClientPlayerManDownMessage`
  248) but not one revive message (`VeniceNetworkReviveInitiatedMessage`, `ServerPlayerReviveMessage`,
  `...ReviveAccepted/Refused`, `ServerPlayerManDownRevivedMessage`: all 0). The exe has a fireteam revive:
  `BuddyReviveComponentData` (`BuddyReviveRadius`, `BuddyReviveTimer`), `InteractiveManDownReviveTime`,
  `PostReviveResponseTime`, `PopupType_RevivePrompt`. To look at: whether a downed bot's soldier stays man-down long
  enough (the bot's 5 s respawn may cut it short), what starts a buddy revive (the prompt never came up), and the
  downed player's accept (`VeniceNetworkReviveResponseMessage`) - a bot has no client to send it.
- **Flashbangs:** `FlashbangConfig`, `FlashBangDistance`, `FlashBangThreshold`, `IsFlashBanged`, `HasStunEffect`,
  `AntHitReactionWeaponType_Flashbang`; the brain knows nothing of them. To look at: where `IsFlashBanged` is set
  (server soldier or only your game's screen effect) - else detect flash explosions and blind bots in range that face
  them.

## Flashbangs and smoke (`src/grenades.cpp`, twenty-sixth round)
`IsFlashBanged` is only an `AnimatedWeaponBinding` field (+0x248, an AntRef); the white-out is your game's screen
effect, so bots get their own. Every server explosion goes through the explosion entity's detonate (`0xBDD840`,
this = `ServerExplosionEntity`: +0x14 data, +0x20 position, +0x40 giver info, +0xD0 / +0xD4 radius / damage scale,
+0xD8 detonated), which dispatches **`ServerCollisionExplosionSpawnMessage`** (category `0xDB60E01E`, type
`0x0376E37B`, 0x50 bytes; ctor `0x259FC60`): +0x28 the explosion entity, +0x2C the giver's player (`0xBAE090`),
+0x30 position (vec4), +0x40 BlastRadius x scale, +0x44 BlastDamage x scale. (~135 a round; also
`ServerMetricsDetonateExplosionMessage`, built by `0xBC4CF0`.) `ExplosionEntityData` (0xD0): BlastDamage +0x9C,
BlastRadius +0xA0, **HasStunEffect +0xC4** (flashbangs), TriggerImpairedHearing +0xC6.
`AirburstExplosionEntityData` (TypeInfo `0x2BA9504`, 0x100) +0xF4 **SmokeScreen** → `SmokeScreenEntityData` (0x90):
ShrinkDelay +0x70, LifeTime +0x74, SpawnDelay +0x78, Radius +0x7C, OccludePlayerSpotting +0x80, RadiusTimeFallOff
+0x81 (`ServerSmokeScreenEntity`, 0xA0, is the live cloud). `GrenadeEntityData` +0x168 GrenadeType.
- The message hook keeps flashbangs (2 s) and smoke clouds (sphere 1 m over the burst; full radius after 2 s of
  growing, from SpawnDelay for LifeTime, shrinking to nothing after ShrinkDelay when it falls off).
- A bot that sees a flashbang (clear ray to it) within `[brain] FlashbangMeters` (15) is blind and deaf for up to
  `FlashbangBlindMs` (5 s): x (1 - (d/range)²), x 1 within 45° of where it looks, down to 0.3 with its back to it;
  under 0.5 s doesn't count. Blind: no perception, target and awareness dropped, no fire, walks back at half speed.
  Any team's flashbang. Logged: `brain X: flashbanged for 3.2 s (6 m away, looking 20 deg off it)`.
- `Visible()` (spotting new enemies and keeping the current one) fails through smoke (`SmokeBlocksSight=1`).
- Logged: the first 12 explosions of a session (`grenades: explosion at (...): blast R m / D damage, stun 0|1, smoke
  screen yes|no`), every `grenades: flashbang at` and `grenades: smoke screen at (...): radius, up N s after N s...`.
MP_10 (Sarajevo Stadium, first round; user 11/7, 627 nodes learned, 272 "no route" while it learned, stuck spells all
freed - longest 30 s): **flashbangs CONFIRMED** - 32 went off (flash data: blast 5.0 m / 1 damage, stun 1; frags
4.5 m / 100-170), mostly bots' own ("grenade at the enemy's cover" 95 times: kits whose grenade is a flashbang), and
bots were blinded 58 times, 0.8-4.9 s (avg 3.1 s; e.g. 2 m, 45° off → 4.9 s; 11 m, back turned → 0.8 s).
**Smoke never came through the explosions** (no airburst data with a SmokeScreen; one 0.4 m / 1 damage blast may have
been a smoke grenade). So smoke now comes from the live entities: every `ServerSmokeScreenEntity` (vtable `0x27B7FC0`,
ctor `0xB3DFD0`, dtor `0xB3A300`) keeps itself in a global eastl::vector (begin `0x2AF1730`, end `0x2AF1734`,
capacity `0x2AF1738`). Fields: +0x14 SmokeScreenEntityData, +0x90 (cleared in the ctor), +0x94 age (s), +0x98
active, +0x9C thrower (weak ref), +0x20 / +0x80 sub-objects (vtables `0x27B7F98` / `0x27B7F7C`). Its radius right
now is `0xB2A070` (thiscall, float): Radius, x (1 - (age - ShrinkDelay) / (LifeTime - ShrinkDelay)) past ShrinkDelay
when RadiusTimeFallOff. Position as `0xEAA160` reads it: `[e+0x1C]` (or, with bit 20 of `[e+8]`, `+0x10 + byte[+6] x
0x50` into it) +0x30. `0xEAA160` is a score check (points to the thrower when a teammate stands in their smoke).
grenades.cpp walks the list (`SmokeBlocks` per sight ray; `ServerTick` logs `grenades: smoke screen up at (...):
radius R m, lasts N s`); imagetest checks the fields and the list.

## Smoke screen support action, a class change, traps as goals (twenty-seventh round)
MP_05 on the twenty-sixth-round build (round over at ~7½ min, user 18/4, no swims). The user's smoke is the **Smoke
Screen support action** (there are no smoke grenades): `ServerPlayerChoseSupportAbilityMessage`, 5 s later
`ServerGameplayMortarStrikeMessage` + `ServerPlayerFiredSupportAbilityMessage`, and 3.6 s after that the entity list
had **one** `ServerSmokeScreenEntity`: `grenades: smoke screen up at (-40.5, 77.0, 8.7): radius 5.0 m, lasts 30.0 s,
shrinking toward the end` - detection CONFIRMED. 5 m is the game's own `SmokeScreenRadius` (what it uses to stop
spotting through it); the visible cloud may be bigger. The log had no way to say whether it hid anyone, so
`SmokeBlocks` now counts per cloud and the cloud's end is logged: `grenades: smoke screen at (...) cleared after 30 s:
it hid N bot sight lines` (a bot looks every 0.15-0.25 s, so one bot staring into it adds ~5 a second).
Flashbangs: 4 went off, 10 bots blinded (1.2-4.3 s).
**Class change CONFIRMED in game:** Sumting Wong, stuck 15 s at (-57.1, 78.4, -3.8) on a route - the round-21 nook,
node 234, every link into it already at 64 so the 9 s trap found nothing new to raise - `changing class to get out`,
died 15 ms later, back 5 s later at his base. Why a route went in: a goal (a roam node, a flank point's nearest node,
an enemy standing in it) could be the trap itself, and then A* has to walk in whatever it costs; and a link learned
into a trap later (bots and you walking in) came in at penalty 1, so traps wore off. Now nodes are **trap spots**
(`MarkTrap`, or on load every link into the node at 64; `nav: ... loaded N nodes, N links, N trap spots`): never a
route's goal (the goal is the nearest node that isn't one), never handed out by `NearestNode` / `RandomNode` /
`NearestNodeAvoiding`, and a link learned into one gets 64. navtest checks both (each fails without its fix). The
round-23 pit (772, 773, 775, 776) and round-24 shore strip (1412, 1416) were hand-marked from outside only, so links
between their own nodes stayed at 1-2 and they didn't count as trap spots: raised to 64 (10 + 2 links; backups
`.before-round28.bak`). Trap spots now: MP_05 234, 772, 773, 775, 776; MP_03 556, 600, 1412, 1416; MP_10 none.

## Shuffling in place and limp bodies (twenty-eighth round)
MP_03 on the twenty-seventh-round build (12¾ min, user 26/5, 1 swim). User: bots piling up, walking and staring into
walls for a while; a couple of dead bots stayed in their ragdoll pose and moved about. Smoke screen: one cloud at
(65.4, 69.2, 21.2), `it hid 0 bot sight lines` (no bot looked through it). Against the last MP_03 round (24th): stuck
520 s in 158 spells (was 396 s / 129), engagements 639 (796); bots in packs of 3+ the same (0-3% a minute after the
first). The new hotspot is the steps / rubble at x 57-61, z -9..+7 (75 s in 8 spells, plus 39 s at (49.9, 68, 0)):
node 977 (61.6, 67.5, -6.4) has cheap links in (1-2) and every bigger way out penalised 8-16. Those spells ran 15-24 s
with only 4 checks finding the bot still (it moved about inside 5 m between them), so the trap (6 stuck checks) and
the class change (10) never came - the "staring into walls". Now a spell also escalates by its length: 12 s into it
with 3 stuck checks the spot is marked as a trap, and `LongSpellChecks` (1.5 x StuckChangeClassSeconds = 22.5 s) into
it with 3 stuck checks the bot changes class (`brain X: 12.0 s without getting 5 m from (...): marked as a trap`;
CheckStuck sets `State::changeClass`).
**Limp bodies** (`src/limp.cpp`, `[bots] LimpBodyKillMs=500`): `JuiceSoldierRagdollActivateMessage` has no payload
(0x28, just the header; sent from `0x7EDEA0`, the ragdoll component); `ClientPlayerManDownMessage` (0x30) carries a
ClientPlayer at +0x28 (built in `0xDC8E40`). Ragdolls still matched kills (246 / 245) and killing blows fired only 2
times, so the client-only kills the combat log catches aren't all of them. Now your game's thread reads every AI
player's soldier health there every 100 ms (ClientSoldierEntity vtable `0x27A09B8`, slot +0xD8 = `0x98E950`:
`[soldier+0xD4]` +0x18, else +0x20 - the same slot as the server soldier's `0xAD4070`; imagetest checks both); a
soldier that has had health there and then reads 0 or below, while the server's soldier for the same player id still
has health after LimpBodyKillMs, is killed on the server (`eng::KillSoldier`): `bot X: your game has shown it dead for
N ms while the server has it alive (health H) - a limp body on your screen: killed on the server too`. Once per body.
Also seen: team 2's support bots do 8 damage a hit against team 1's 21 (rounds 27 and 28 alike: Doug 0 kills in
12 min, 41 hits for 284 damage).

## Team 2's bullets did 40% (BotBulletsAsPlayers)
Not the kit: both teams' bots come from the one TeamData (`MOHW/Gameplay/Teams/TeamAllies`, "Task Force Atlas", six
kits, 12 UnitData; extracted from MP_03's bundles - `kit_support`'s weapon parts are the same for both). Not health
either: the soldier's health sum (`0xB67E00`) takes DifficultyData's HumanHealthModifier (+0x38) for you,
FriendsHealthModifier (+0x34) for AI on team 1 and EnemiesHealthModifier (+0x3C) for AI on team 2
(`mohw/settings/difficultysettings`: friends 1.0 at every difficulty, enemies 0.75 / 1.0 / 1.5), and the combat log
counts health actually lost. It's the damage sum shared by the server and your game (`0xD3CE00`; server `0xE3CBB0` ->
`0xD3F930` -> `0xD3F7E0` -> here, your game `0xDCC8B0` the same way): for an AI shooter (+0x354) it multiplies the
damage by the shooter's current weapon's `GameAIWeaponData` (WeaponEntityData +0x78) through the soldier's +0x130
interface, slot +0x74 (`0xB5ECB0`: soldier +0x5C0 weapons -> current weapon `0xB74380` -> data +0x14 -> AIData +0x78;
`FriendlyAIBulletDamageMultiplier` +0x10 when the shooter's team (+0x38C) is 1, else `BulletDamageMultiplier` +0x0C) -
the campaign's weaker enemies. The support LMG's must be ~0.4 (snipers' ~1: their hits matched across teams). Also in
that sum: an AI shooter on team 1 with +0x388 set against a target whose vfunc +0x6C says so gets health + 1 (an
instant kill), you against AI x10 for one damage type (+0x38 param 1, unless the soldier's +0x11F), the shooter
player's own multiplier (+0x356 flag, +0x358), SyncedGameSettings.BulletDamageModifier, then the victim's +0x6C
component and DifficultyData. Now `[bots] BotBulletsAsPlayers=1` (default) makes the AI step's `je` (0xD3CF22, `74 2C`)
a `jmp`: bot bullets do what a player's do, on both teams, on bots and on you (`SIG Damage_aiWeaponMultiplier`;
imagetest checks the jump, its target and the multiplier function). Expect team 2 to fight back - and hit you harder.

## Twenty-ninth round (MP_10, all three fixes in)
Sarajevo Stadium, ~9½ min to the timer, user 7/5. **BotBulletsAsPlayers CONFIRMED:** team 2's support bots 20 a hit
(were 8), team 1's 24; kills team 1 137 / team 2 122, deaths 130 / 142 (the two MP_03 / MP_05 rounds before: 142-150
kills against 71-89). **Limp bodies CONFIRMED firing:** 9 bots shown dead on your screen 500-516 ms while the server
had them at 4-59 health, all in close fights (2-5 m), each killed on the server 15 ms later; killing blows fired 3
times besides. Those deaths have no killer (KillSoldier is a suicide), so 9 kills went uncredited. **Stuck by time:**
3 spots marked by the 12 s rule (Barttle Doo (85.9, 88.6, 161.7) 3 links, Sumting Wong (102.0, 88.0, 149.6) 4, Billy
(109.8, 91.1, 86.4) 11 - 3 m from a team 2 spawn point, crouched in an animation (a vault), 13 stuck lines there this
round); no class change needed (longest spell 19.5 s, was 37.5 s on the first MP_10 round); stuck 430 s in 146 spells
(556 / 171), "no route" 2 (272 while the graph was new). No swims, no smoke used, 22 flashbangs / 43 blindings.

## Thirtieth round (MP_05): pile-ups made traps, a class change after getting free
Novi Grad, timer, user 20/9 (top of the board); kills team 1 150 / team 2 136, deaths 150 / 159. **Smoke CONFIRMED
blocking sight:** one Smoke Screen at (-56.9, 77.9, 4.0) hid 404 bot sight lines in its 30 s. Limp bodies 13, *all on
team 1* (round 29: 7 of 9) - team 2 lost 13 kills to them (136 would have been 149); why your game kills team 1's bots
early is not known yet. 12 flashbangs / 27 blindings, no swims, "no route" 19 (23 last time here, nearly all toward
team 1's spawn at (-100, 80, -26)).

Stuck 438 s in 119 spells (236 / 85 on the last Novi Grad round), and the 12 s rule marked 8 spots - 17 nodes, with
10-16 route links into a spot: busy junctions in team 2's half. In 5 of the 8 another bot was stuck within 4 m at the
same moment, and a third of all stuck time was like that (r28: 16 of 214 s): **bots pushing at each other.** Line of
sight and the probes (`Clear`) ignore characters, so no bot ever steered round another, and each pushing match counted
as being stuck on the map. Also **Dingus changed class at the check that found it 7 m away, free** ("stuck 0.0 s of
0.0"): the long-spell rule set `changeClass` before the spell's end was checked.

Fixes:
- `CheckStuck` works out whether the spell ends (`away`) first; the 12 s trap and the 22.5 s class change only apply
  on a check the spell goes on past.
- `NearestBody`: a soldier within 2 m and ahead within 75 degrees makes the bot veer 1 rad off its side for 0.6 s
  (dead ahead always the same turn, so a head-on pair passes).
- A stuck check with a soldier within 1.5 m isn't counted: no streak, no route penalty, no trap, no class change.
  Half the time the bot also backs off 1-1.8 s, so two bots head-on in a corridor don't sidestep into its walls together
  for good.
- MP_05's nav: the 12 nodes of the 5 pile-up spots unmarked (their 69 in-links back to 1.00; backup
  `.before-round32.bak`). 10 trap nodes stay: the 5 from before and 5 from three lone bots' spells (Slick twice, by old
  trap 234 where it later changed class, and Wang Chung).

## The game's own alive check: limp bodies at the root, stealth melee, revive (after the thirtieth round)
All three below are read from the code and pass imagetest; **none has been seen in a game yet.**

**`[bots] BotHealthFromServer=1` (default): why a bot could be dead only on your screen.** Your game has a health state
machine per soldier (10 states, vtables `0x2813B08`..`0x2814188`; module vtable `0x2814A18`, the module at soldier
+0x54C). Every state's update first asks `0xDCFC20`: "AI soldier (or no player), and AllowClientSideDamageArbitration
on?". Yes: it reads the soldier's vtable +0xD8, your game's **own estimate**. No: it reads module +0x5AC, **the server's
health** (vtable +0x7C, `0xDD55F0`), which every ghost update writes (`0xDCF250` -> `0xDCF1B0`). The estimate is the
server's health plus your game's hits the server hasn't acknowledged (`0xDCEED0`; up to 32 kept at module +0x4C, added
in `0xDCC8B0`, dropped when the ghost state's ids at +0x60 name them). `0xDCC8B0` is reached from the soldier's onDamage
(vtable 0x27A09B8 +0x104, `0x964380`), which first asks `0x962DB0(victim, giver)` whether your game handles the hit: yes
for your own shots, no for a remote human's, and for an AI or no giver on an AI victim yes (client [+0x38]+0xA). Since
the fourteenth round the server ignores claims of bots hitting bots, so those hits are never acknowledged: the estimate
sinks below the server's health, reaches 0, and the state machine ragdolls a bot the server has alive. That's the limp
body, and it can't recover: `0xDD5F40` comes back from man down on health > 0, but keeps reading the estimate.
The patch makes a bot behave as a remote human's soldier does, three sites, all or nothing:
- `0x962E20`, the jne for an AI victim now reaches `xor al, al; ret 8` (`0x962DD4`): bot-on-bot hits aren't your game's;
  no estimate, no claim, so the killing-blow code in combatlog.cpp has nothing left to do.
- `0xDCFC20` -> `xor eax, eax; ret`: the states always read the server's copy.
- `0xDCF4C9`, jne -> nops: a bot's ghost update keeps the server's last-hit data, which remote soldiers use and bots had
  from their own hits.
Your own shots at bots still go through the estimate and a claim, as against a real player. Since the bot dies of real
damage, the kill has a real killer: the uncredited kills go away with it. The all-on-team-1 pattern of the last round
is still unexplained, but no longer matters if this holds. limp.cpp now only watches (`limp::WatchOnly`): it logs
`your game's estimate has had it dead ... nothing should show` (expect ~0 now) and kills nothing. New checks of what the
fix relies on: `your game's copy of the server's health follows it` (once, the first hit seen arrive) and `... has been
off for 1 s` (should never come). Evidence the copy works for bots: bots killed by the server alone have always
ragdolled, and that only reaches your game through this copy. `BotHealthFromServer=0` restores the old behaviour with
LimpBodyKillMs.

**`[bots] StealthMeleeOnBots=1` (default), `src/melee.cpp`.** The instant kill is the game's "humiliation melee".
`0x981870` (ClientMeleeComponent, soldier +0x420, each frame) takes the target from +0x234 (from the aim simulation,
`0xA10B20` via `0xA1F8EA`), checks range, pose and angle (`0x97AD90`), then at `0x981986` accepts a human victim at
once; an AI one only if its melee component has a state element set (`0x7EDBD0`: inferred to be PreparedToDefendMelee,
what a campaign script sets up), which a bot never has. Without a victim your melee is the normal hit. The `je` becomes
`jmp`. No AI or coop test was found on the server's side (`0xB846B0`, `0xB7BC40`, `ServerMeleeManager::start`
`0xB844D0`; not exhaustive). The server's kill (`0xB79780`) does the victim's whole health with you as the giver,
flags not client-arbitrable, so the kill should score normally. While `0xB73D40` says a bot is in one, it gets a neutral
input, as the game's own AI brain does (`0x1854F3B`). **Not proven:** whether the aim simulation ever targets a bot.
The log says: `melee: your melee can target a bot` (first seen) and `melee: the server starts a stealth kill: you ... on
a bot ...`. A target but no start means another check; no target means calling `0xB844D0` from the DLL instead.

**Revive: there's no check to flip, multiplayer has it switched off for everyone.** The mpsoldier EBX has
`ManDownStateTime` 0, `InteractiveManDown` False, and `BuddyReviveComponentData` is `Excluded: True` with no code reading
it. The server's state table (`0xE459B0`) has ManDown -> Revive on event 7, which nothing raises in that state (the only
`push 7`, `0xE3981B`, needs InteractiveManDown; computed pushes not ruled out). `VeniceNetworkReviveFinished/Response`
have no senders. 0 revive lines in the last 8 sessions' logs. Fastest health gain between hits 3.2/s against
RegenerationRate 3.5, so no buddy heal either. What looked like your buddy reviving you is most likely a spawn on your
buddy (40 of 70 spawns land within 9 m, with the deploy camera on the buddy), not proven. Reviving a bot would be a
feature to build, not an unlock: hold a downed bot's state (replace `0x281AA08`, ManDown update `0xE36020`), raise event
7 from a human holding EIAInteract nearby, inject input 35 for the accept (`0xE3F8E0`), freeze the brain and respawn,
undo the scoreboard kill. All read, none run.

**Decision: reviving is not being built.** You were down, your buddy moved by and you got back up, which you put down to
the avenge bonus's fast respawn (not checked against the game). The notes above stay for reference only.

## Thirty-first round (MP_03): no limp bodies, melee targets bots, no stealth kill seen yet
MP_03 on the build above (about 12 minutes, you 33/8, your team won 150-80). You called it really smooth.

- **Limp bodies:** no `estimate has had it dead` line, and the copy of the server's health never read 0 with the bot alive
  on the server, apart from one false line at a respawn (the 1 s timer isn't reset when the soldier changes: Drumstick,
  "22 there" then "0 there" against 100, 30 ms after its new soldier appeared, 5 s after it died). Kills 230, deaths 235,
  so 5 deaths without a killer (the round before: 286 / 309, 13 of them limp kills).
- **The copy lags:** 19 `off for 1 s` lines. 10 are about 10 HP low while the server regenerates; 7 read 100 while the
  server has 8-88 (only Ooga Booga, Barttle Doo and Wang Chung; cause not looked into); 2 are the respawn above. Nothing
  visible came of them that you reported.
- **Stealth melee:** `melee: your melee can target a bot` at 12:28:13, so the aim sim does pick bots. That was around when
  you shot Cornwallas at 8 m, so its target may not need melee range. No `the server starts a stealth kill`; the log can't
  say whether you tried one.
- **Stuck:** 148 spells, 488 s (the last MP_03: 158, 520 s), longest 22.5 s, no class changes. 45 spells had another stuck
  bot within 4 m (177 of 396 s, against 28 and 69 s), so the sidestep doesn't show in these numbers.
- **Marks, all left in place:** three at one spot, (55.5, 68, 0.9), (56.4, 68, 5.3), (56.2, 69.3, 8.3), nodes 1695, 1696,
  1699, 481: spell starts there went from 20 in the first 4 minutes to 1 in the last 9 (the last MP_03: 20 spread over the
  whole round). A spot not seen before took over from minute 6, x -5..12, z -20..-8: 20 spell starts, against 1 and 3 in
  the last two MP_03 rounds, possibly traffic moved there. Node 36 joins 556 and 600 at (17-20, 64-65, 5-9); node 843 is a
  bot falling in the bay. Traps now 11: 36, 481, 556, 600, 843, 1412, 1415, 1416, 1695, 1696, 1699.
- **Score gap:** 150-80 is like the last MP_03 (150-89), so not from these patches.

## ProCon / RCON (`src/rconsrv.cpp`, `src/rcon.cpp`, `[rcon]` in the ini)
ProCon talks to a dedicated server's remote administration port. lanmohw.exe keeps the settings for it (`ServerSettings`
`AdministrationEnabled`, `RemoteAdministrationPort`) but none of the command handlers, which were in the separate
dedicated server exe. So the DLL has a small Frostbite RCON server of its own, on 127.0.0.1:47200 by default, answering as
`MOHW` build 323544 (R-10, the last in ProCon's `Configs\MOHW.def`). With `Password` empty any login is accepted.

**Wire format:** u32 header, u32 total size, u32 word count, then per word u32 length, bytes, NUL. In the header bit 31 means
the exchange started on the server, bit 30 that this is the response, the rest is the sequence number. So a reply to ProCon
has bit 30 only; an event has bit 31. `login.hashed`: no argument returns a hex salt, then MD5(salt bytes + password) in hex
capitals (checked against ProCon's own hash).

**ProCon's MOHW parsing, read from `PRoCon.Core.dll` with ildasm:**
- `mapList.list`: `OK <map count> <dropped> <words per map> <map> <mode> <rounds>...`. It drops the second word, unlike BF3's
  format; with BF3's the list stays empty.
- `serverInfo`: name, players, max, mode, map, round, rounds, `2 <score> <score> <target>`, state, ranked, punkbuster, passworded,
  uptime, round time, ip:port, pb version, queue, region, ping site, country, quick match.
- `player.onKill <killer> <victim> <weapon> <headshot>`, `player.onSpawn <name>`, `player.onJoin <name> <guid>`,
  `player.onChat <name> <text>`; the rest as on BF3. `player.onLeave <name> <player block>`.
- It asks for each `vars.*` once at login and polls `serverInfo` and `admin.listPlayers all` about every 30 s, so the player list
  is a little behind between events.

**What it does in the game:** the player list (bots by name, kills / deaths / points from the scoreboard rows), team scores (the
sum of each team's kills), target (75 x `GameModeCounterModifier`, Team Deathmatch), events for join, leave, spawn, kill, team and
squad changes, chat (`ServerPlayerChatMessage`: +0x28 player, +0x2C text), level loaded and round over.
- `admin.killPlayer`: `KillSoldier`.
- `admin.kickPlayer`: bots only. The bot dies and the game's own AI death path deletes its player (`ShouldDeleteAiPlayer`
  answers yes for it); `FillTeams` then adds a new bot.
- `admin.say`: `0x873290` with a made-up network chat message, channel 4: the game's own ADMIN line (`0xF55397`), on your HUD.
- `mapList.setNextMapIndex`: writes `NextMap` in `mohw-bots-state.ini`, so it takes effect on the next launch.
- `mapList.runNextRound` / `restartRound` (build 2026.11.09, `src/relaunch.cpp`): the game restarts with the next entry (or, for a
  restart, this map and mode as a one-off `[launch] Once` in the state file, which `ChooseMap` plays without moving the turn). Chat
  and your HUD say what comes; 3 s later the routes are saved, `CreateProcess` starts `lanmohw.exe` again with only its quoted path
  as the command line (any argument breaks the offline start) and `MOHW_BOTS_AFTER_PID=<this pid>` in its environment, and this
  game ends with `TerminateProcess` (not `ExitProcess`, whose static destructors run with the game's threads gone). The new game's
  DllMain clears the variable and waits up to 20 s for the old process before it binds the RCON port. ProCon's Next Round button
  (`uscServerConnection.btnNextRound_Click`, after an "are you sure") and a double-click in the Maplist tab (`setNextMapIndex`, then
  `runNextRound`) send these; ProCon's `ReconnectVersionChecker` reconnects auto-connect servers every 20 s.
  `tools/smoketest/relaunchtest.cpp` checks the hand-off (no arguments, game folder, variable cleared, old process gone first).
  Since 2026.11.09.2 the restart is the fallback; first the level changes in place (below).
- **In-place level change** (2026.11.09.2, `src/levelchange.cpp`, `[rcon] NextRoundInPlace`, NOT played yet). The server's
  handler for a client's `NetworkRequestLoadLevelMessage` (`0xAA8900`, from the server's network listener `0xAA8E20`) checks the
  sender's connection, then queues a `ServerLoadLevelMessage` (category `0xC6E40BA0`, type `0xF8429A0E`, 0x48 bytes from the
  server arena `0x2A59130`, base ctor `0x259EB00`, vtable `0x27AD710`, the `LevelSetup` copied to `+0x28` by `0x258FE50`, `+0x40`
  = 0) 0.6 s ahead (`GameContext::queueMessage` `0x588360`) and dispatches `ServerRoundResetMessage`. `eng::RequestLevelLoad` does
  the same minus the connection check. The `LevelSetup` (28 bytes: Name, InclusionOptions, DifficultyIndex, SubLevelNames,
  SubLevelStates, MOHSpeedCheckpoint, ForceReloadResources +0x18, IsSaveGame) is built inside a `NetworkRequestLoadLevelMessage`
  (ctor `0x258C490`, setup `+0x38`) the way the client's own "load level by name and mode" (`0xF62DB0`) does: `CString::assign`
  `0x527A10` for the name, `0x7E7C20(setup, "GameMode", mode)`, the difficulty from `SyncedGameSettings +0x18` (the handler resets
  synced settings when it differs); destroyed with `0x40A9D0` and `0x6284E0`. The server's handler (`0xAAB140`) ignores it while
  a load is under way (`+0x41`, `+0x6D`); with a level in (`+0x7C`) it compares setups (`0x627C00`) and skips the load when they
  match and `ForceReloadResources` is clear, so a restart, or the same map in another mode, sets it; otherwise it marks the load
  (`+0x6D`) and starts it through `[server+0x60]` vtable +4, then loads with `ServerInternalLoadLevelMessage` once that says it's
  ready. Your game should follow over its connection (`NetworkLoadLevelMessage`, handled at `0x880B10`), as on any online server.
  Before asking, the DLL switches `GameSettings.DefaultLayerInclusion` (the buffer it already points the game at), the map name it
  logs and the mode rules; bots stand still (`levelchange::Pending`) and get fresh brains when the new level's key shows. Done =
  the server's level object changed (or was gone and came back) with the new key, and your game's level (`[0x2AC5FE4] +0x10 +0x68`,
  logged once as `your game's level reads ...`) is the new map; then `NextMap` moves on and chat says `Now playing ...`. Gives up
  and restarts the game with that map (`[launch] Once`) if the server doesn't have it in 150 s, your game doesn't follow in 90 s, or
  no server tick comes for 3 minutes (a watchdog thread). The server's load flags are logged whenever they change. `imagetest`
  checks the request's setup layout and both messages' hashes with the game's own constructors; the rest needs a play test.
  **First play (2026.11.09.2, Basilan Combat Mission reloaded):** the change itself worked (server in 1.4 s, your game in 35 s,
  deploy screen), then every bot respawn faulted at `0x48B6F0` (`CharacterPhysicsEntity` +0x84, null: the new soldier's
  `soldier+0x1A4`, read by `0xB5B4B0` from the soldier setup `0xE16790` under `0xE1EA23`) and the game froze 15 s later, on the
  fourth round of 19. On the unload all 20 players got `ServerPlayerReleasingLevelMessage` but only you `ServerPlayerEnteredLevel`;
  KeepPlayers had kept the bot players (an unpatched game deletes an AI player whose soldier goes), and they still held the old
  level's soldier blueprint, kit and unlocks (`setSelectedSoldierAsset`, the default loadout), which the full reload had freed.
  Every faulted spawn had made a soldier entity first and left it half set up. **Since 2026.11.09.3** a change has two parts: the
  bots leave (`levelchange::BotsLeaving`, last in the bots' tick): a living bot's soldier is killed and, while a change is pending,
  `ShouldDeleteAiPlayer` lets the game's own death path delete the player; a dead bot's player is deleted by `eng::DeleteAiPlayer`
  with the same two calls (`0xB42D40` releases the loadout, `ServerPlayerManager::deletePlayer` `0xB3AC90` on `context +0x30`,
  which also serves a human leaving while dead). Each comes back as a new player with its name and team 5 s after the change is
  in (`g_respawns`, made from the new level's TeamData). Once no bot player is left the mode is switched and the load asked for;
  bots still there after 3 s restart the game instead. Nav skips the old level while it is read with the new mode
  (`levelchange::LeavingLevel`). And two bot spawns faulting on one level stop bot spawns there (`g_spawnsHalted`); on a level
  that came in place the game restarts with it.
  **Your points (2026.11.10.2).** The server context stays across an in-place change, so `scoreboard::Reset` never ran: the game
  started your kills and deaths again, but `scoreboard::Tick` kept writing the points it had for you (2026-11-10: 24/5/2900 at the
  end of MP_21 Sector Control, 0/0/2900 on Hot Spot's first rows, your game's scoreboard too). `StartLoad` now calls
  `scoreboard::NewLevel` once the load is asked for: it forgets every row's points, clears the sub-round flag and sets the points
  of the records left (yours; the bots' went with their players) to 0. Log: `scoreboard: new level - points start from 0`.
  **Your game's arrival (2026.11.12.3).** 2026-11-12 19:54, MP_19 TDM → MP_21 SC: the server had the level after 7.7 s and you
  were playing it, but `level change: your game has ...` never came, the bots stayed out, and after 90 s the game restarted.
  `ClientTick` (on your game's input polls) took its baseline on its first poll after the request; you sat on the deploy
  screen, which doesn't poll, so its first look was at MP_21 itself (`your game's level reads "MOHW/Levels/MP/MP_21/MP_21"`,
  first logged at 19:55:06), and that level never differed from the baseline. The baseline (level and its name) is now taken
  when the change is asked for, on the server thread, and a level with the target's name counts as arrived whenever the
  baseline was another map (only Restart Round still needs the level to change or vanish).
- **Round end, kicks and the hang at the end of a session** (2026.11.09.4). Offline MOHW never moves on from a round by itself:
  after `ServerRoundOverMessage` (the 10-minute timer or the kill target) the results screen stays up (every such session in the
  log ended with you quitting within a minute). Every "next round" the log shows came after the server **kicked you**: a team
  kill in Hardcore (`ServerPlayerTeamKillKickMessage`, disconnect code 21) or the idle kick (`ServerPlayerNoInteractivityKickMessage`,
  code 19, ~5 minutes on the deploy screen). The server then ends the round with nobody in it, your game exits to the menu,
  sends `ClientStartMultiplayerMessage` again and gets a new server (`ServerGameContext changed`); the bots were standing down
  for the round over, so none respawned in between. On 11-09 16:29 (Hot Spot, 2026.11.09.3) the idle kick came mid-round: one
  bot's 5 s respawn fell 8 ms after the server dropped the level, faulted at `0x4CAD41` (the soldier setup reads the server's
  world table `[0x2A4F724 + realm*4]`, empty without a level), the server stopped 4 s later and the game hung. Now:
  a bot due to respawn waits while `TeamDataFor` finds no level (`bot X: no level to spawn into`); the kicks are off by default
  (`[launch] IdleKick=0`, `TeamKillKick=0`: `BFServerSettings` +0x1C `NoInteractivityTimeoutTime` -> 0, which the check
  `0xE1E000` reads as off below 0.001, and +0x28 / +0x2C `TeamKillCountForKick` / `TeamKillValueForKick` -> 0, which `0xE726F0`
  skips); and a round over with you still in the game starts ProCon's Next Round after `[launch] NextRoundSeconds` (30;
  `rcon::Tick`, needs `[rcon] Enabled=1`), cancelled if a new round starts, you leave or a map change is already asked for.
  Not explained yet: in that Hot Spot round you died at ~16:24:55 and never deployed again (no deploy-screen input reached
  the game for 4 minutes) while the server and your game ran on.
- `vars.*` for settings (friendly fire, regeneration, bullet damage, game mode counter, health, respawn time, man down time, HUD
  flags, kill cam): read from and written to the settings objects. The others read as fixed values.
- Not there: `mapList.endRound` (answers `NotSupportedOnThisServer`), moving players, bans, reserved slots.

**Testing without the game:** `build/smoketest/Release/rcontest.exe [port] [password] [log]` runs the server with made-up players and
events. Pointing a copy of ProCon at it, with the real `PRoCon.Core.dll` loaded in a small harness, showed login with a password,
serverInfo, 20 players, 3 map entries, indices, every event and every error reply parsed, with nothing in `DEBUG.txt`.

## Hardcore mode (`src/hardcore.cpp`, `[hardcore]` in the ini)
A copy of BF3's Hardcore preset, off by default. BF3's values come from the 4netplayers preset table
(<https://www.4netplayers.com/en/wiki/battlefield-3/game-mode-presets/>), the only full list found. All the settings are read live.

| BF3 | Warfighter | Who reads it |
|---|---|---|
| `friendlyFire true` | `SyncedGameSettings.EnableFriendlyFire` (+0x1D) | server `0xBB45C7`, `0xBC969C`, `0xE06084`; no AI test seen |
| `regenerateHealth false` | `SyncedGameSettings.DisableRegenerateHealth` (+0x1C) | server `0xBB861F` exits early; no AI test seen |
| `soldierHealth 60` | `ServerSettings.HumanHealthMultiplier` (+0x2C) = 0.6 | `0xB67EB6`, human only until patched |
| `killCam false` | `SyncedMOHWSettings.NoKillCam` | `0x170FF9F` |
| `hud false` | `HudNoCrosshair`, `ExplosiveIcons`, `HealthAmmo`, `PointsTracker`, `Obituary`, `GameMode`, `BuddyInfo`, `Unlocks`, `ClassAbility` | HUD, each its own flag |
| `miniMap true`, `miniMapSpotting true` | left as they are | |
| `3dSpotting false` | `SyncedBFSettings.No3dSpotting` | HUD by name, `0xF54A81` |
| `nameTag false` | `NoNameTag`, `HudNoEnemyNametag`, and `NoBuddyOutline` (Warfighter's own, my addition) | HUD |
| `onlySquadLeaderSpawn true` | `SyncedBFSettings.OnlySquadLeaderSpawn` | spawn screen `0xF614FF`; bots in `TrySpawnOnBuddy` (extent +0xF8 = leader) |
| `3pCam`, `vehicleSpawnAllowed`, `playerRespawnTime`, `playerManDownTime`, `bulletDamage` | no change | |

- **Bot health:** `0xB67E00` multiplies in the team factor and `HumanHealthMultiplier` only if the soldier isn't AI. In the thirty-second round
  `vars.soldierHealth 70` gave you 70 and the bots stayed at 100. Patched so an AI soldier gets the setting but still skips the team factor,
  in any mode, so ProCon's `vars.soldierHealth` reaches bots too. It's only read at spawn.
- **Bots and friendly fire:** while the setting is on, whoever set it, a bot doesn't fire while a teammate is within 0.6 m + 2 cm per metre of its
  line to the target, and doesn't throw a grenade with a teammate within 7 m of the target. `team kill:` lines count what still happens.
- `ForceMinimapSpotting` leaves `No3dSpotting` alone when Hardcore holds it. `hardcore::Apply` runs every 2 s from `ApplyOverrides` and logs anything
  it had to put back. A value set from ProCon is left alone for the rest of the launch.
- Thirty-second round (MP_05): friendly fire was on from ProCon for nearly all of it, with 12 deaths without an enemy killer against 5 the round before.
  That isn't proof bots hurt each other; the `team kill:` lines are there to settle it.

**Switching it from ProCon.** ProCon's settings panels are fixed per game, so the switch is a plugin, `package/CMohwBots.cs` (goes in ProCon's `Plugins\MOHW`). It sends
`vars.preset hardcore|normal` (BF4's name for this; also works typed in ProCon's console). `hardcore::Set` builds the rules from `[hardcore]`, or puts back what each setting
was before Hardcore changed it, and writes `Enabled` to the ini so the next launch keeps it. The game is the authority: it adds ` [Hardcore]` to the server name, the plugin reads
that from `serverInfo` (about every 30 s), and the value ProCon restores from its own config at startup never changes the game. After a switch the plugin asks for the changed
`vars.*` again 3 s later, because ProCon only asks once at login and its panels would stay stale. Rules are behind a lock: the switch runs on the server thread, `Apply` on either.

**The Gameplay page's Hardcore box (plugin 1.1.0, build 2026.11.07.5).** `uscServerSettingsGameplayMOHW`'s constructor sets `chkSettingsHardcore.Enabled = false`, and
`MOHWClient.FetchStartupVariables` never asks `vars.hardCore` (BFBC2 did), so ProCon keeps it greyed. A page box is unlocked by `uscPage.OnSettingResponse`, which runs when
ProCon reads a reply for that var, whoever asked. So the game now answers and accepts `vars.hardCore` (same switch as `vars.preset`), and the plugin asks for it on enable, on
login, and when the server name's mark disagrees with what it knows. It switches with `vars.hardCore` too, so the box follows the plugin. `OnHardcore` (raised for any
`vars.hardCore` reply, including the box's own set) is how the plugin hears a click on the box; it then asks for the changed `vars.*` again. An older game answers
`UnknownCommand`; `OnResponseError` then falls back to the server name and `vars.preset`.
Tested with ProCon's real page: `tools/procon-rig/gameplay-page-test.cs` loads `PRoCon.exe`, hosts the page in a form, connects through the real
`PRoCon.Core` to `rcontest`, flips boxes the way a click does and screenshots the page. Box greyed with the plugin off; clickable and ticked with it on; Killcam followed a
Hardcore switch; Killcam set by hand while Hardcore was on stuck; the console's `vars.preset normal` unticked the box after the next `serverInfo`. With `RCONTEST_OLD=1` the
plugin's own switch still worked through `vars.preset`.
The other boxes on the page already worked: they use `vars.killCam`, `vars.hud*`, `vars.buddyOutline` and so on, which are in `kSettingVars`. `vars.autoBalance`, `vars.3pCam`,
`vars.roundStartPlayerCount` and `vars.roundRestartPlayerCount` are read-only placeholders, so ProCon shows them failing. The Gameplay Presets list (Normal, Immersive) is
ProCon's own: it sends the single vars, not a preset.

With ProCon's plugin sandbox on (`runPluginsInSandbox True`, its default), this PC's .NET 4.8.1 refuses `CPRoConPluginLoaderFactory` (`Inheritance security rules violated ... InitializeLifetimeService`), so no plugin loads at all, ProCon's own included. Reproduced with the same `PRoCon.Core.dll`; with the sandbox off the plugin loads and works. The test copy of ProCon had it off, which hid this. It needs Options > Plugins > Plugin security > "Run plugins with no restrictions".

## Game modes (`src/modes.cpp`, `src/objectives.cpp`, build 2026.11.07.6, not played yet)
A mode is a sub-level the map includes for the `GameMode` criterion (`[launch] Mode`, written into
`GameSettings.DefaultLayerInclusion`). The criterion's values (logged: `layer inclusion criterion GameMode: ...`) are TeamDeathMatch,
CombatMission, CaptureTheFlag, SectorControl, ObjectiveRaid, ObjectiveRaid_1Life, SectorControl_1Life, Sport, TeamDeathMatch_Fireteam,
FireTeam_Survivor, BombSquad. The exe's UI names six (`ID_M_GAMEMODE_*`) and ProCon's `MOHW.def` five: TeamDeathMatch, CombatMission,
SectorControl, BombSquad ("Hot Spot") and Sport ("Home Run"; its stat events and HUD strings are flag attack / defend rounds with
elimination). Which sub-levels each map ships, from its decoded `.toc` (`mp_xx_tdm`, `_cm`, `_sc`, `_sm`/`_sport`, `_bs`/`_bombsquad`):

| Map | TDM | Combat Mission | Sector Control | Home Run | Hot Spot |
|---|---|---|---|---|---|
| MP_03 Somalia Stronghold | yes | yes | yes | yes | yes |
| MP_05 Novi Grad Warzone | yes | yes | yes | no | yes |
| MP_10 Sarajevo Stadium | yes | yes | yes | yes | yes |
| MP_12 Basilan Aftermath | yes | yes | yes | yes | no |

That matches `MOHW.def` for all ten maps, so `modes.cpp` uses its list. `[launch] Map` entries take a mode after a colon
(`MP_03:SectorControl`); an entry without one plays `[launch] Mode`, one with a mode its map lacks plays Team Deathmatch (logged).

**ProCon's Maplist tab.** For MOHW the list is "open" (`MOHWClient.HasOpenMaplist`): the pool shows every mode's maps from `MOHW.def`
and adds with `mapList.add <map> <mode> <rounds>` (rounds 2 when the box is 0), then `mapList.save`; it removes with
`mapList.remove <index>`, reorders by remove + add with an index, and sets the next map with `mapList.setNextMapIndex`. `rconsrv` now
owns the rotation: it validates through `Config::checkMap` (`InvalidMap`, `InvalidGameModeOnMap`), keeps the next index on the same
entry across inserts and removes, and queues `mapList.persist <next> <map> <mode>...` with the whole list, which `rcon.cpp` writes to
`[launch] Map` and `NextMap`. So the list is saved as you edit it, for the next launch; `runNextRound` / `restartRound` restart the
game with it (ProCon / RCON section). `admin.setPlaylist` / `admin.getPlaylist` / `vars.playlist` answer too (BFClient's playlist
handlers; the tab only uses them for games without an open list). Tested with ProCon's own `uscMaplist` against `rcontest`
(`tools/procon-rig/maplist-test.cs`): the list shows each entry's mode, adding Sector Control on Basilan and Home Run on Sarajevo works,
Hot Spot on a map the test server doesn't know comes back as an error, set next and remove land in the saved list.

**What other modes change for the bots.** The own-base spawn override and the camped-base shared spawns are Team Deathmatch only now
(Sector Control's `TacticalSpawnPointChooserEntityData` has `TeamFilter` on already, and its flag spawners change team as flags are
taken); in other modes the game's chooser decides. Home Run is one life per round: a bot that dies waits for the next round change
(`ServerRoundResetMessage`, `ServerRoundSubRoundOverMessage` or `ServerRoundHalfTimeMessage`; a soldier lost within 10 s after one went
with the round change), and those messages are logged (`round: sub-round over`). A mode's route file (`MP_03_SectorControl.nav`)
starts as a copy of the map's Team Deathmatch routes. The kill target in `serverInfo` is Team Deathmatch only; the team scores are
still kills.

**Sector Control flags.** The SC sub-level parks its three control points at one holder spot inside
`sectorcontrol/sectorcontrol_fullprefab` (on MP_03 three points 3 m apart at (29-35, 64, -25)). The real places come in through the
sub-level's interface: `LinearTransform` field values, property-connected to the fullprefab instance (fields -285332114, -728925637,
901000446 on every map), which hands each to one control point prefab (plain = A, `_var_b_center` = B, `_var_c_north` = C). World =
value x the fullprefab's placement. `python tools/objectives.py --sc-flags MP_03 ... --inc` writes them into `src/objectives.inc`;
MP_03's B lands within 1 m of the layer's `OBBData` capture shape and its flag spawners. Two more connected transforms per map are
the team bases (`pf_initialspawnmarker_sc`). **Since 2026.11.10.2 all ten maps are in the table.** MP_18 and MP_19 name the SC bundle
differently (`mp_18sc`, `mp19_sc`; `sc_bundle` matches `mp_?N_?sc`). MP_20 and MP_21 are the expansion's: their .toc and .sb are in
`Update/Xpack1/Data/Win32` (`level_toc` looks in Patch, then the other Update folders, then the base game), and that superbundle
isn't cas: each bundle is BF3's binary bundle inside the .sb (`fb2_sbtoc.noncas_ebx`: magic `0x970D1C13`, payloads in cas block
format, 16-byte aligned; all 834 EBX of `mp_21_sc` decode). MP_21 played as Team Deathmatch on 2026-11-10 for want of them; its
three flags now land 1.0-3.1 m from nodes walked in that round (`MP_21_SectorControl.nav`). MP_18 / MP_19 / MP_20 are unchecked in
play (MP_19's TDM routes cover too little of the map to compare).

`ServerCapturePointEntity` (0x104 bytes, vtable `0x281B450`, ctor `0xE450D0` (creationInfo, data) ret 8, dtor `0xE442E0`) has no
position (how it learns who is inside isn't known yet; see "The pre-round" below). `+0xA8` owner team (`setTeam` `0xE3A590`,
ghost bit 1), `+0xAC` the team whose flag is up, `+0xC0` / `+0xC4` capture progress and flag height (ghost bit 2, sent / 100), `+0x94`
a vector cleared on reset. The capture radius is 5 m, 20 s to take, `SeesawCapturing`. `objectives.cpp` hooks the ctor and dtor to
keep a list, reads the owners every 0.5 s, and learns which entity is which flag the first time an entity's progress moves while
soldiers stand at exactly one unlinked flag (the last pair links itself). Until then a flag's owner is what all unlinked entities say
if they agree (all neutral at the start), else unknown. Log lines: `objectives: Sector Control flags A (...)`, `flag B is capture
point ...`, `flag B taken by team 2`, a per-minute owner summary, and in every mode `N capture point entities in <level>`.

**Played 2026-11-12 19:54 (2026.11.12.2, MP_21): captures work.** At 19:57:01 bots of both teams reached their base flags
(A up to 4 of team 1, C up to 6 of team 2): control at once, the flags 2 → 100 in 10 s (faster with more inside), then
`flag C captured by team 2`, `flag A captured by team 1` and the game's own `ServerGameplayCapturePointCapturedMessage`;
B went to team 1 at 19:58:06. Two faults: a flag left on its way up sank back (`ReturnMultiplier`, -8/100 a step) and at
the bottom went to the *other* team (A, left at 92/100, to team 2, who had never been there), because the attacker passed
with nobody inside was the controller's opponent; since 2026.11.12.3 it is 0 then, so it ends with nobody in control. And
the 2026.11.12.2 pre-round watch read the old level's freed pre-round entity 5 times during the in-place change
(`ENGINE FAULT in ReadPtr`, guarded): the pre-round hooks and logging are gone in 2026.11.12.3 (the image test checks
both functions are untouched). Seen in play: the flags stand at the top of their poles at the start, although the server
has them at the bottom (`StartAtBottom`, +0xC4 = 0). **Why (2026.11.13.1):** your game's `ClientCapturePointEntity` (ctor
`0xDDA810`) starts its shown height (+0xC8) from `StartAtBottom` too (0, else 1) and its target (+0xD4, the server's / 100) at
0; its update (`0xDD2390`) eases the one toward the other and writes it to the flag only when it changes (output property
0xD16667B4 through +0x80, `0x577FB0`), so a flag nobody has touched is never placed and keeps its prefab's place, the top. (A
flag of another kind is placed when `0xDD6410` spawns it from `FlagTemplates`, which returns at once for `CapturableType` 3.)
`DriveCaptures` now raises each flag at rest with nobody on it to 1/100 for a second once you have had a soldier 3 s since
its capture points appeared (your game has them then), skipping its capture step meanwhile, then sets it back to 0: two
changes your game follows down to the bottom. Logged once a level: `flags at rest raised 1/100 for a second ...`.

**Who captures (2026.11.12.2).** 2026.11.12.1's logging (MP_18 SC from launch): the pre-round exists
(`entity ... created`) but reads `wants 0 player(s), has 0 entered, state 0x10`. It never listens (bit 2 of +0xB8 is its
"registered for player messages" flag, cleared by `0xED1E50`), and it doesn't need to: the interface's empty
`RoundStartPlayerCount` reaches it as 0, and its first update (`0xED1F10`, timer +0xB4 from `0xED61A0`) runs `0xECE890`,
which fires `StartRound` (0xA41E7427) and sends `VeniceServerMetricsStartRoundMessage`, its only sender, 50 ms after the
entity is made, in every session's log. **So rounds always started; the pre-round paragraphs below were wrong.** The capture
points: their whole 0x104 bytes changed only at +0x28..+0x40 and +0x6C (set-up) in 7 minutes of bots standing on them. The
reason: `ServerCapturePointEntity`'s update slot (vtable +0x88, `0xE36A60`) is a bare `ret 4`, its message listener a bare
`ret 4`, and its capture step `0xE47F30` (and the BF3-style `0xE47300`) are called from nowhere in the exe, any section. In
this build nothing ever runs a capture. `0xE47F30(float dt, {ServerPlayer** begin, end}* inside, int counts[], int attacker)`
ret 0x10: `0xE3A230` turns counts into a rate (counts[0] the soldiers inside that aren't the controlling team's (+0xA8),
counts[1] the controller's, then per-squad counts at +8.. and +0x20.. for the squad bonus via `0xE3A040`; with
`SeesawCapturing`, attackers ahead by at least `MinNrToTakeControl` give a negative rate, holders ahead a positive one, capped at
`MaxCaptureMultiplier` and scaled by `AddedMultiplierPerPlayer`; nobody inside: `0xE36B20`, the flag going back at
`ReturnMultiplier`). Rate > 0: `0xE47940` raises the flag (+0xC4 += rate x dt / `TimeToGetControl`) and at the top fires
OnCaptured (0x7D1F6304 as a stat event, 0x4BF96304), sets +0xAC = +0xA8, marks the ghost dirty and sends
`ServerGameplayCapturePointCapturedMessage`. Rate < 0: `0xE47B90` lowers it (`TimeToLoseControl`), sends
`ServerGameplayCapturePointLostMessage` (+0x2C the attacker) if it was up, and at the bottom `0xE46810` and `setTeam(attacker)`
(`0xE3A590`). Both send an unnamed progress message (type 0xD34EDB3E, vtable 0x281ADA0: +0x28 entity, +0x2C delta, +0x30 the
inside list, +0x34 counts), whose only listener is the scoring system (`0xE81C23` → `0xE7F6A0`), which walks the list
(team at `player+0x38C`) to award capture points. `objectives.cpp` now links each entity to its flag from its data's
`FlagID` (+0xE0; A 0, B 1, C 2 on every map) and, every 0.5 s in Sector Control, counts the soldiers with a soldier within
`CaptureRadius` (+0x8C, 5 m flat, 3 m up or down) of each flag and calls `0xE47F30` (`eng::CapturePointUpdate`, SEH-guarded;
a fault stops it for the session), with nobody in control the bigger team attacking. Logged: `flag A: 2 of team 1 and 0 of
team 2 inside (nobody in control)`, `flag A: team 1 in control`, `flag A captured by team 1`. A flag counts as a team's
(for the bots and the spawn rule) once that team is in control and its flag is up (+0xA8 == +0xAC).

**Spawns in Sector Control (2026.11.12.2).** "Bots shouldn't even spawn at the enemy's initial base flag, even if being camped."
None did in the two rounds logged (by nearest flag: team 1 at A, team 2 at C and its base, four of team 2 near B), but the
flag spawners change team with their flag, so taking the enemy's base flag would give a team points there. A team's
starting base is where its first 10 fall-back spawns of the level landed; the enemy's base is within 40 m of their middle or
25 m of the flag nearest it (if within 60 m). While a bot's team holds that flag its fall-back spawns use its own starting
spots (`eng::SpawnAt`, one with no enemy within 25 m), and a buddy in the enemy's base isn't spawned on; a game pick that
still lands there is logged. The points' own positions can't be used: the flag spawners are one prefab placed three times,
so their data positions are local.

**Played 2026-11-12 (2026.11.11.1, MP_20 TDM then MP_21 Sector Control in place): no `pre-round:` line at all**, so the
listener was never called; the three capture points read fine (`owner nobody, nobody's flag up, capture 0, flag at 0`)
and stayed so with you standing on a flag, and no `ServerGameplaySetPreRoundLogicMessage`. 2026.11.12.1 hooks the
constructor too (`0xEDED10`; logs `pre-round: entity ... created` and each one's count, list size and state as they change,
lowering the count there as well) to learn whether a `ServerPreRoundEntity` exists at all, and logs every capture point
whole once (`capture point ... raw:`, 65 words) and then which words change (`raw change: +C0 00000000>...`, every 3 s at
most, 400 lines a level), to see which fields react, if any, while someone stands on it. The data, read since: each point
is `CaptureRadius` 5 m around its own transform, `MinNrToTakeControl` 1, `TimeToGetControl` 20 s, `CapturableType` 3
(`CTSectorControl`: 0 default, 1 unable to change team, 2 capture enablable), and each control point prefab is moved into
place by a property connection to its `BlueprintTransform` (field -409930682) from the fullprefab's interface.

**The pre-round (2026.11.11.1).** First Sector Control play with flags (2026-11-11, MP_21 then MP_18,
both after an in-place change): in 15 minutes no capture point moved (no owner, progress or flag height change, so no flag
linked), your own capture did nothing, and the bots mostly stayed at their base. Every mode's sub-level starts its round
from `pf_preround_simple` → `pf_preround_wrapper` → `PreRoundEntityData` (`RoundStartPlayerCount` 8, `RoundStartCountdown`
10, on every map and mode; no level data sends its skip input, hash 180587226, nor runs the console command `release`).
Its `StartRound` is what starts Sector Control (`sectorcontrol`: `OnStartRound` to `sectorcontrol_scoring`'s ticket
counters and `sectorcontrol_server`'s state machine), Combat Mission's and Team Deathmatch's state machines and round
timers, and Hot Spot's site chooser. `ServerPreRoundEntity` (0xBC, vtable `0x2822F28`, ctor `0xEDED10`) has its message
listener at +0x90 (`0xEDEF20`): `ServerPlayerEnteredLevelMessage` adds the player to a list (+0x94), `ReleasingLevel` takes
it out, and on those and on `ServerPlayerChangedSoldierMessage` it checks `0xECE720` (listed players with a soldier,
`player+0x42C`) >= +0xA4 and `0xECE780` (each team, `player+0x38C`, has +0xA4 / 2), then starts the countdown (+0xB8
bit 0, events `0x2CBFEA2D`). The countdown's end sends `StatEnableMessage` and `ServerGameplaySetPreRoundLogicMessage`
(`0xECE810`); the latter **never appears in any session's log**, so offline no round has ever started. The reason: a
player enters the level in `ServerPlayer::update` (`0xB445B0` → `0xB2EF90`) only once +0x504 (`0xB2F490`, level loaded,
`ServerPlayerLevelLoadedMessage`) and +0x505 (`0xB2F4E0`, extents told) are set, and both come from the player's
`ServerConnection` (`0xAF5F40`, `0xAF5A70`). Bots have none, so each level logs one `EnteredLevel` (you) and twenty
`ReleasingLevel`. `objectives.cpp` now hooks the listener and sets +0xA4 to 1 before it runs (logged `pre-round: the level
wants 8 players ...`), so the countdown starts when you first spawn (`pre-round: enough players; the round starts in 10 s`),
with the countdown's input restriction (no moving or firing for 10 s) on everyone. Not done: making bots enter the level
(calling `0xB2F490` / `0xB2F4E0` for them would also run the player extents' level-ready code on players with no connection).
Whether `ServerCapturePointEntity` itself waits for the round is not known (its message listener `0xE36D10` is a bare
`ret 4`; `SetPreRoundLogic`'s only listener is `ServerRoundOverEntity`, `0xEDD340`), so the capture points now log their
progress and flag height as they change (`objectives: flag A capture 40/100, flag at 60/100 ...`) and their starting
values in the `N capture point entities` line. The control point prefab's two `GeometryTriggerEntityData` are `RunOnce`
first-entry triggers for the HUD and voice lines, not what the capture counts.

**The brain** has a fourth goal, objective (`brain::SetObjectives`): with no target, a bot heads for a flag its team doesn't hold (or an
own flag with an enemy within 15 m; a few guard their own), roamers 75% of the time, flankers 60%, hunters 45%, nearer flags likelier.
It keeps at it (85% per re-pick) until the flag is its team's; a fight interrupts it. **Since 2026.11.12.1** (2026-11-12:
on a flag the bots "run in circles and are spastic", and both teams mostly camped the flag at their spawn) it walks to a
spot within 2.4 m and stands there, watching a way in (`WatchYaw`: of six learned nodes 10-35 m from the flag, the best
of: one it can see +1, toward a flag its team doesn't hold up to +0.6, not the way it just watched -0.8, plus up to 0.8
at random; a new one every 2.5-5 s, a noise it hears first), and every 8-18 s, half the time, shifts to another spot.
Standing there on purpose doesn't count as stuck. A flag's weight is divided by 1 + 0.3 per teammate within 12 m of it,
so a team spreads over the flags (its spawn crowd no longer pulls everyone to the nearest); once a bot has stood a minute
on a flag that still isn't its team's, it keeps at it 30% of re-picks instead of 85% and weighs that flag x0.2 (on its
own flag, defending, that never runs).
The spread line counts objective goals. Combat Mission, Hot Spot and Home Run objectives (bomb sites are `GameInteractionEntityData`,
`HoldToInteractTime`, `EIAInteract` 33) aren't played by bots yet.

## Resetting a map's routes
`package/reset-bot-routes.bat` (game closed; asks which map, or takes `MP_03`) renames `mohw-bots-nav\<map>_*.nav` to `.reset<n>.bak`.
With the game running, `nav.reset [map]` from ProCon's console: `nav::ResetMap` renames to `.reset-<time>.bak`, and if that's the map being played
the graph empties at once. A bot's route in hand is positions plus node numbers that only `Penalize` uses, and it checks them, so bots walk out their
last route and then find no route until the graph grows. The map can be a code or any part of its name. The result goes to ProCon's chat and your HUD.

## Packaging for testers
`python tools/package.py` writes `dist/mohw-bots-<version>.zip`: the DLL from `build/Release`, `dll/mohw-bots.ini`, the `.nav` files from
the game folder (not the `.bak` copies) and `package/README.txt`, with the exe's size and SHA-256 filled in. Change `MOHW_BOTS_VERSION` in
`dll/src/version.h` and rebuild for each build you hand out; the log shows it on the line after `mohw-bots loaded`. The script stops if a
file holds this PC's user name or an email address, or a text file holds a drive path.

No game files go in. Testers need the game and the LAN pack (`lanmohw.exe`, `MOHW.exe`, `dinput8.dll`, `BFUlauncher.exe`, from
`mohw_client.exe`), the same `lanmohw.exe` build. On any other build the signature check turns the bots off and the game runs as normal.

## Spawn camping (sixteenth round)
Your team "camped the enemy spawn". The log agrees: team 2 died 79 times to team 1's 42 (you had 22 of the kills),
median life 28 s against 62 s, and 25 of team 2's deaths (32%) came within 20 s of spawning (team 1: 4). All 112
fall-back spawns used the bot's own base (team 1's 10 points, team 2's 10; none ever ran out), plus 19 buddy spawns.
With own-base-only fall-backs a pushed team has nowhere else to come in; the shared mid-map points (25 of 49) are the
obvious relief valve if it keeps happening.

## Noticing and hearing (ninth round)
Before, a bot "saw" any enemy in its 120° view the first time a ray reached them (every 0.15-0.25 s) and turned to
them at once - only the first shot waited for the reaction time. Anyone within 5 m counted as heard from any
direction, whatever they were doing. Now:
- **Noticing takes time in sight:** each sight check adds `interval / NoticeTime` to that enemy's awareness; at 1 the
  bot notices (turns, then the reaction time before shooting). Out of sight it fades over 3 s, so peeking again
  picks up where it left off. `NoticeTime` = `NoticeMs` (450) × (1.5 − skill) × up to 3 at the edge of the view ×
  (1 + (distance − 10 m) / 25 m) × 1.5 crouched / 2.5 prone × 1.3 standing still / 0.8 sprinting × 0.35 while
  firing. E.g. you running at 20 m straight ahead: ~0.7 s, then 0.4-0.9 s to the first shot; crouched and still at
  25 m, 45° off: ~3 s.
- **Hearing turns the head, it doesn't notice you:** an enemy outside the view within `HearGunfireMeters` (30, while
  firing), `HearSprintMeters` (10), `HearRunMeters` (5, upright) or `HearWalkMeters` (2; crouched-and-slow or prone is
  silent) makes the bot look that way for 1.5 s (at most every 3 s, not mid-fight; logged `heard ... turning to
  look`). It still has to see and notice you. Speed comes from each soldier's position over time, gunfire from the
  shot messages (`spotting::SinceFired`).
- `PeripheralDelayMs`, `NoticeMsPer10m` and `HearingMeters` are gone (folded into the above).
- **Spawn protection (tenth round):** your fall-back spawn put you 13 m from an enemy bot with a line of sight to you;
  it engaged 0.65 s later. The game's chooser (`finalFilterVisibility` `0xEE4550`) does test every living enemy
  player, bots included (eye → spawn point), so that pick was visible to it too or became so a moment later. Now a
  soldier younger than `SpawnGraceMs` (default 2000) can't be noticed at all; noticing and the reaction time come on
  top, so the first shot at a fresh spawn comes ~3 s or more after it.

## Spotting / minimap (`src/spotting.cpp`, `[spotting]` in the ini)
- **What the diagnostics showed (fourth round):** bot shots do carry the bot player (`ServerWeaponPlayerPrimaryFire
  ShotSpawnedMessage +0x28`, controlled = soldier), yet the generic `SpottingTargetComponent::spot` (`0xE0AA00`, the
  FB "spot on fire") was never called - not for your shots, not for your game's 30+ passive spot reports. MOHW soldiers
  carry `MOHWServerSpottingTargetComponent` (vtable `0x28CD740`) instead, whose listener (`0x169F340`) handles only
  `ServerPlayerKilledMessage` (victim spot), `VeniceNetworkSpottingMessage` (`0x169DAB0`: a client's spot report,
  type 1 active / 2 passive) and `VeniceNetworkOrderMessage`. Nothing on the server reacts to gunfire, and spot
  reports come only from players' games - bots have none, so friendly bots never spotted for your team.
- **The component:** `+0x08` → owner holder → the soldier, `+0x04` data (`MOHWSpottingTargetComponentData`: `+0x60`
  ActiveSpottedTime, `+0x64` PassiveSpottedTime, `+0x80` VictimSpottedTime, `+0x88` MQ9SpottedTime, `+0x90`
  FlashSpottedTime), five replicated lists from `+0x34` (`SpottedType` 0 active, 1 passive, 2 victim, 3 MQ9, 4 flash;
  entries {weak spotter, timer}, dirty bits 1..0x10), and `+0x2C` / `+0x30` an **unspottable** flag and timer
  (`0x168E660`, set by `VeniceExplosionEntityData.UnspotsOnExplode` / `UnspottableTime` - smoke).
- **The fix:** `getComponent<MOHWServerSpottingTargetComponent>(soldier)` (`0xE02780`) and its own
  `addSpot(spotterPlayer, SpottedType)` (`0x169C220`, what an explosion's `SpotTypeOnExplode` calls: refreshes the
  spotter's entry or adds one with the type's time × the target's multiplier, marks the list dirty). Every tick:
  an enemy bot that fired (`RevealOnFire`) is spotted by you with `FireSpotType` (default 3, the MQ-9 UAV's
  minimap-only spot); an enemy a friendly bot can see (`FriendlyBotsSpot`) is spotted by that bot with `SightSpotType`
  (default 1, passive, as a human teammate's game reports). At most once a second per target per reason. The first
  dozen spots are logged with the component's spot times and the server list sizes.
- **Fifth round:** every addSpot succeeded (spot times: active 5 s, passive 1 s, MQ9 10 s) and still nothing on the
  minimap. The client side: the component's network state (`0x16917B0`, per list: count, then spotter player id +
  timer per entry) is read by `0x16B59C0` into `MOHWClientSpottingTargetComponent` lists (`+0x18` unspottable, lists
  at `+0x20` active, `+0x30` passive, `+0x40` victim, `+0x50` MQ9, `+0x60` flash, `+0x70` local; the spotter is
  looked up with `ClientPlayerManager::getPlayer(id)` `0x901750`, an entry is dropped if that fails). The minimap
  (`0x1786230`, per client player → `0x17850A0`) shows an enemy whose active or MQ9 list has entries (or victim list
  with you as spotter); the passive list isn't consulted at all. So the server MQ9 spots should have shown - somewhere
  between the server lists and the client lists they're lost. Two additions: (1) **client fallback**: each server
  spot also queues the target's player id; on your game's main thread (the XInput poll - the thread that dispatches
  client messages) the DLL finds that player's client soldier (`0x901750` → `ClientPlayer::getSoldier` `0x903B80` →
  `getComponent<MOHWClientSpottingTargetComponent>` `0xDE88B0`) and calls the client's own addSpot (`0x16B5D90`,
  what your game does when you spot someone) with `ClientSpotType` (default 3) and you as spotter; (2) **client
  diagnostics** every 5 s: client player counts per team and the list sizes of every enemy with spots
  (`spotting (your game): ...`).
- **Sixth round:** no enemy bots on the minimap even with the UAV support action, and no `spotting (your game)` line
  at all: the game doesn't poll XInput during a match, so the client tick never ran. Server lists kept MQ9 entries
  (refreshed while the bot fired), and the per-minute counts showed 126 `ClientGameplaySpottedEnemyMessage` - raised
  only by the client read (`0x16B5CA7`) when a new spot lands in a client list - so the spots do reach your game. The
  minimap (`0x1786230`) loops over `ClientPlayerManager +0x98`, takes each player's soldier (`ClientPlayer +0x42C`
  weak handle, `0x903B80`) and skips players without one; with even UAV spots invisible, the likely gap is bot
  ClientPlayers without a linked soldier (or not in that list). The client tick now runs from the message hook for
  every message the client's own message manager (`[[0x2AC5FE4]]`) dispatches, and the diagnostics list every client
  player (`[id team lists]`, `no-soldier`, `no-spot-component`) to settle it.
- **Seventh round:** the client diagnostics settled the pipeline: all 12 players are in your game's list, every bot
  ClientPlayer has its soldier and spotting component, enemy bots carry MQ9 entries (from replication and the client
  fallback; e.g. five enemies spotted at once), and your own soldier link is there while you're alive - yet nothing
  on the minimap. The minimap is `MOHWIGHUDMinimapComp` (vtable `0x28E7A48`, update `0x1786DD0` → `0x1786C70`: needs
  your team, your soldier and HUD state `0x2A2E87C` != -1, then the per-player icon pass `0x1786230` → `0x17850A0`,
  which shows an enemy whose active / MQ9 list has entries, or who fired recently - the client soldier's fire-event
  list `+0x7F8`, `MOHWUIMinimapCompData.MinimapEnemyFiringThreshold` / `MinimapEnemyFadeTime`). What hides spotted
  enemies on the HUD is the game preset: `SyncedBFSettings` (TypeInfo `0x2B949B4`) `NoMinimap` +0x10,
  `NoMinimapSpotting` +0x12, `No3dSpotting` +0x13, and `SyncedMOHWSettings.HudNoMinimap` (+0x0C), synced to clients
  (`0xE40880` → `0xE2D6A0`) and read by the HUD by name through `0xF54A20`. `[spotting] ForceMinimapSpotting=1`
  logs them once (`game preset minimap flags as found: ...`) and forces them off (re-checked every 2 s, as the server
  re-syncs the preset). The client diagnostics also log the minimap's gate (your soldier, HUD state).
- **Eighth round:** the preset flags were already all 0 (`NoMinimap=0, NoMinimapSpotting=0, No3dSpotting=0,
  HudNoMinimap=0`); HUD state 2 in play; enemies kept MQ9 entries in your game - and still no icons. Now the minimap
  itself is instrumented: hooks on the per-player pass (`0x1786230`) and the icon update (`0x17850A0`) count, every
  diagnostics line, the passes, the enemy icon updates with their flags (a5, a6, MQ9, active) and how many left the
  icon visible (icon list at `minimap+0x224`, icon `+8` soldier, `+1` active, `+0x44` alpha), and each enemy's
  per-player gate (`soldier+0xB0` vfunc `+0x1C`, `gate-closed` when false).
- **Ninth round - the cause:** the instrumentation showed ~300 icon passes per 5 s, no gate closed, and **0 enemy icon
  updates** - the pass never got as far as the icon for an enemy bot. In the pass, right after the per-player gate:
  `lea ecx, [soldier+0xB0]; call [vtable+0x24]` (`0x9550B0`, isAI: the player's `+0x354`), `je 0x178636B` (bytes
  `74 0E` at `0x178635B`), then `mov eax, [esp+0x3C]` (your team), `cmp [esp+0x14], eax` (its team), `jne skip`. An
  **enemy AI soldier is never drawn** - a campaign rule (no minimap icons for enemy AI); friendly AI is. Bots are AI
  players (that's what makes the server apply their hits and step them), so no spot, UAV or gunfire could ever show
  them. `[spotting] ShowEnemyBotsOnMinimap=1` (default) turns the `je` into `jmp` (`EB 0E`, signature
  `Minimap_skipEnemyAI`): enemy bots now go through the same checks as enemy humans - shown while spotted (MQ9 or
  active list: gunfire via `RevealOnFire`, the UAV, teammates) or firing. Logged as `minimap enemy-AI skip patched`;
  the image smoketest checks the patched bytes. The server spots were reaching your game all along, so the client
  fallback (`ClientSpots`) is now off by default.

## Fireteams (`[bots] Fireteams`)
A player's fireteam is the squad id in their `VeniceServerPlayerExtent` (`+0xF4`, `5` = none; set to 5 by the extent
ctor `0xE1BE70`); `getFireteamBuddy` (`0xE3A8A0`) returns the first teammate with the same id - including 5. Online
the server's squad handling assigns ids; offline nothing does (the fourth-round log: 112 bot spawns found "you" as
their buddy, 77 a non-mutual one), so the buddy was the first teammate in the player list, changing with every death,
and the buddy score bonus went to that player. `AssignFireteams` pairs players each tick with the game's own
`setSquad(squad, team)` (`0xE1C850`: leader flags, `ServerPlayerSwitchSquadMessage`, dirty bit 1 at extent `+4`):
you first, into a fireteam with the next unpaired bot of your team; the other bots pairwise (5 fireteams per team);
a respawned bot rejoins its old fireteam (by name). Logged as `fireteams: Bot03 (team 1) -> fireteam 0`.
- **Buddy points:** the scoring system's buddy branch (`0xE7F130` after `0xE7E4E0`) has no AI check on the scorer:
  for a kill by a bot whose buddy is you, the DLL sends the score event too - the bot scores nothing, you get "+10
  ID_IGHUD_SCORE_BUDDY_KILL" (within 20 m, the game's rule) and the collection adds 10 to your meter. The buddy text
  is passed as an eastl::string {begin, end, capacity, allocator 0}.

## Navigation (`src/nav.cpp`, `[nav]` in the ini)
MOHW's MP levels ship no navmesh; fun-bots uses hand-recorded paths. Here the paths are learned automatically from
human players:
- **Learning** (`nav::Learn`, every server tick for each living human, and each bot with `LearnFromBots=1`): a node is dropped when the walker is
  `NodeSpacingCm` (2.5 m) from every node within 1.2 m of height, and linked to the node they came from; walking past
  an existing node (within 1.75 m) links into it, which joins routes into one graph. No nodes mid-air (|vertical
  speed| > 2.5 m/s); a move of more than 5 m in one tick (respawn) starts a new chain. A link that climbs > 0.6 m (or
  where the walker went up in the air) is a **jump link**; the reverse direction is only added if the height
  difference is ≤ 1.2 m (no walking back up a ledge you dropped from).
- **Storage:** `mohw-bots-nav\<level>_<GameMode>.nav` next to the DLL (text: nodes, then links with flags and
  penalty), key from the server level's name (`level+0x68`) and `GameSettings.DefaultLayerInclusion`; loaded on level
  change (checked every 5 s), saved every `SaveSeconds` while it changes and when the map changes. 4 m spatial grid.
- **Following** (`brain.cpp`): while roaming, bots plan an A* route (cost = length × penalty) from the node nearest
  them (≤ 6 m, else 12 m; ranked by distance + 2 × height difference) to the node nearest the goal (≤ 12 m), re-planned
  every 5 s and on every new goal; on the ground they advance when within 1.4 m of a waypoint or past it - never past
  one more than 1.4 m above their feet - press jump approaching a jump link, and walk the last stretch to the goal
  directly. On a route, obstacle probes only jump (no detours off the route). Stuck on a link → that link's penalty
  doubles (max 64, saved) and the bot re-plans; stuck before the first waypoint, or still stuck after 4.5 s of new
  routes → no route for 4 s (detours); stuck 9 s → the spot is a trap (every link into it at 64). Without a route
  the old behaviour applies.
- **Offline test:** `build/smoketest/Release/navtest.exe` walks simulated players along an L corridor, a ledge and a
  parallel corridor and checks node spacing, node reuse, routes around corners both ways, jump links, disconnected
  areas, penalties re-routing, save/load (with penalties), respawn chains, starting on the bot's own floor and
  routing around a marked trap.

### Message trace
`MessageManager::dispatch` (`0x5925C0`) is always hooked (offline scoring, bot spotting); `[debug] TraceMessages=1` logs the first dispatch of each message
type with its name — the quickest way to see which step of a load/join stops. Every `NetworkDisconnectedMessage`
is also logged with its reason text (`msg detail: … reason "Timed out"`), and `UIExitToMenuReasonMessage` with its
payload whenever the reason changes. `ClientConnection::onDisconnected` (`0x873E80`, vtable `0x2793EC8` slot 5) is
hooked too: it logs the reason code and text plus the return addresses on the stack (`disconnect: … return
addresses:`), which name the code that dropped the connection. Once a minute it also logs how many messages of each
type were dispatched in that minute (`message counts, last minute (N types):`, most frequent first).

### If the game crashes
Windows keeps full dumps in `%LOCALAPPDATA%\CrashDumps\lanmohw.exe.<pid>.dmp`. `tools/crashdump.py <dump>` prints the
exception, registers, and the return addresses on the faulting stack (labelled from `re/labels.json`).
If you can't spawn yourself on the MP map, F8 still works (engine spawn point) and F11 still dumps.

**Multiplayer with other people** needs a Blaze backend (the LAN pack only redirects to one via `-blazeIp`).

| Key | Does |
|---|---|
| F6 | add a bot on your team at a spawn point the engine chooses |
| F7 | add an enemy bot 3 m in front of your soldier (you must be spawned) |
| F8 | add an enemy bot at a spawn point the engine chooses (`ServerPlayerForceRespawnMessage`) |
| F9 | pause / resume the bot brain (paused bots stand still and hold fire) |
| F11 | dump every player (AI flag, team, soldier, position) to the log |

Everything goes to `mohw-bots.log` in the game folder. A good first run looks like:
```
==== mohw-bots loaded into ...\MOHW.exe (image base 00400000) ====
signature check: all match (18 functions)
hook: Server::update @ 00AA8DE0 -> detour ...
first Server::update tick (server ...)
add bot Bot01: ... -> team 2 (spawn in front of host)
  TeamData ...: Soldier=... CustomizedSoldier=... kits=N
  [Bot01] ServerPlayer ...
  [Bot01] spawn call returned; soldier now 0x...
bot Bot01: has soldier ... at (x, y, z)
```

### Reading the result
- **No log file at all** → the proxy didn't load (check the DLL is next to the exe the game actually runs).
- **`signature check: FAILED`** → a different exe than `lanmohw.exe` is running.
- **No `first Server::update tick`** → you're not hosting, or the server runs in another exe/process.
- **`ENGINE FAULT in …`** → that step's assumptions are wrong; send the log.
- **Soldier stays null** after F8 → the spawn-point chooser path rejected the bot; try F7.
- **Bot spawns but doesn't move on F9** → input isn't being consumed for AI players; next build will call
  `singleStepEntry` every tick as a fallback.
