#pragma once

// Raises the client/server connection timeouts (default 5 s) so a local client survives the ~13 s an MP level
// takes to load on the in-process server. The client copies the values into its connection when it connects
// (~6.8 s after start), so they are applied from the earliest main-thread callback we get: XInput polling (~4 s).
// seconds <= 0 disables the override.
//
// layerInclusion ("Criterion=Value;..." e.g. "GameMode=TeamDeathMatch"; empty = leave it) is written into
// GameSettings.DefaultLayerInclusion at the same time. 0x7E7E20 parses it into the LevelSetup of every
// ClientStartMultiplayerMessage (~6.5 s), and the in-process server loads the sub-levels (game-mode layer: teams,
// spawns, rules) that match it.
void InitOverrides(int timeoutSeconds, const char* layerInclusion, bool minimapSpotting);

// Cheap; call often (XInput calls on the main thread, Server::update on the server thread).
void ApplyOverrides();

// The map this launch starts ([launch] Map, e.g. "MP_05"); once in game (your game's thread, ClientMapNameTick) its
// name is logged from the game's own text (ID_M_LEVEL_NAME_<code>).
void SetLaunchMap(const char* code);  // also after an in-place level change: logs the new map's name once it's in
// An in-place level change to another mode: GameSettings.DefaultLayerInclusion becomes this ("GameMode=SectorControl").
void ChangeLayerInclusion(const char* layerInclusion);
void ClientMapNameTick();

// SyncedBFSettings.GameModeCounterModifier as a percentage (BF3's vars.gameModeCounter): the mode's win target is
// scaled by it - Team Deathmatch's 75 kills per team. 0 = the game's own (100%).
void SetGameModeCounter(int percent);

// The server's kicks of a human player (BFServerSettings): false forces one off. Offline you are the only human, so a kick
// ends the match: the server ends the round, your game drops to the menu and builds a new server. The idle kick
// (NoInteractivityTimeoutTime -> 0) came after ~5 minutes on the deploy screen; the team-kill kick (TeamKillCountForKick
// and TeamKillValueForKick -> 0) after a team kill in Hardcore.
void SetHumanKicks(bool idleKick, bool teamKillKick);
