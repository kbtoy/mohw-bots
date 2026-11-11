#pragma once
// Minimap spotting for bots. MOHW soldiers carry a MOHWServerSpottingTargetComponent whose spot lists (active, passive,
// victim, MQ9, flash) are replicated to clients and drive the minimap. They're filled by spot reports from each
// player's game - a human client reports the enemies it sees (passive) or tags (active) - by explosions
// (SpotTypeOnExplode) and support actions; nothing on the server reacts to gunfire. Bots have no game client, so
// friendly bots never spotted anything for your team and enemy bots never showed up on your minimap. This calls the
// component's own addSpot (0x169C220) for them: an enemy bot that fires is spotted for you, and an enemy a friendly
// bot can see is spotted by that bot, as a human teammate's game would report it.

#include <vector>

#include "brain.h"

namespace spotting {

// A friendly bot (spotter player) seeing an enemy (target player).
struct Sighting
{
    void* spotter = nullptr;
    void* target = nullptr;
};

void Init();  // reads [spotting] from mohw-bots.ini

// From the MessageManager::dispatch hook (any thread): records who fired (ServerWeaponPlayerPrimaryFireShotSpawned).
void OnMessage(const void* message);

// Seconds since `player` last fired a shot (99 if not in the last minute); the bots hear gunfire with it.
float SinceFired(void* player);

// Server thread, once a tick: spots enemy bots that fired since the last tick (for `human`) and the enemies in
// `sightings`. Each target is spotted at most once a second per reason; the game keeps the spot for the type's time.
void Tick(void* human, int humanTeam, const std::vector<brain::Actor>& actors, const std::vector<Sighting>& sightings,
          float dt);

// Your game's main thread: spots the same targets in your game directly, and logs what your game's spotting lists
// hold. Driven by OnDispatch (from the message hook, for every message the client's own message manager dispatches;
// the game doesn't poll XInput during a match) and the XInput poll.
void ClientTick();
void OnDispatch(void* messageManager);

}  // namespace spotting
