#pragma once
// ProCon's Next Round / Restart Round without closing the game: the server loads the next level in place, the way it does
// for a client's NetworkRequestLoadLevelMessage (eng::RequestLevelLoad), and your game follows over its connection.
// First the bots leave: a bot's player holds the old level's soldier blueprint, kit and unlock assets, which a reload frees,
// and a respawn from them faulted inside the game (a soldier with no physics) until the game froze. So bot players are
// deleted, as the game itself does with a dead AI player, and come back as new players on the new level. Bots stand still
// while it happens. If the change doesn't arrive in time - bots that won't leave, the server never getting the new level,
// or your game not following - it falls back to restarting the game with that map (relaunch.h). [rcon]
// NextRoundInPlace=0 always restarts instead.

#include <cstddef>
#include <string>

#include "modes.h"

namespace levelchange {

// Server thread. Starts a change to `target` in place; sameLevel reloads the level being played (Restart Round).
// nextMapAfter: what [launch] NextMap becomes once the change is in (-1 = leave it). The bots leave first, then the level
// is asked for (a failure there restarts the game). False if a change can't start; the caller then restarts the game.
bool Request(const modes::Entry& target, bool sameLevel, int nextMapAfter);

// A change is under way: bots don't move, fight, respawn or get added, and a dead bot's player is deleted (KeepPlayers off).
bool Pending();

// The bots are leaving (the first part of a change): kill each bot's soldier and delete the players of dead ones.
bool BotsLeaving();

// `level` (the server's) is the one being left: the new level has been asked for and the server still has the old one. The
// new mode is set already, so a key read from it now would mix the two (nav skips it).
bool LeavingLevel(void* level);

// Server thread, every tick: once no bot player is left (botPlayers) asks for the level, then watches it come in (and
// gives up on it, see above). levelKey as nav's.
void ServerTick(void* server, void* context, const std::string& levelKey, size_t botPlayers);

// Your game's thread: watches your game's level.
void ClientTick();

// Restart the game with `target` (what the fallback does; also what Next Round does with NextRoundInPlace=0).
// Returns only if the game couldn't be started again.
void RestartWith(const modes::Entry& target, int nextMapAfter);

// Bot spawns keep faulting inside the game on this level. If the level was loaded in place, restarts the game with it
// (a fresh start). Returns only if it didn't; the caller then stops spawning bots on this level.
void RestartAfterSpawnFaults();

}  // namespace levelchange
