#pragma once

namespace bots {

// Reads mohw-bots.ini (next to the DLL) and logs the active settings.
void Init();

// Called from the Server::update detour after the original ran, on the server thread.
void OnServerUpdate(void* server, void* updateInfo);

// From the MessageManager::dispatch hook (any thread): the round ending (bots stand down, no respawns) and a new
// round starting.
void OnMessage(const void* message);

// Server thread: a bot of ours leaves at its next death, by the game's own path (a new bot fills its place). False
// for anyone else.
bool Kick(void* player);

}  // namespace bots
