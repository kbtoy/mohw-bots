#pragma once
// Combat statistics from the game's own events: who fires (ServerWeaponPlayerPrimaryFireShotSpawnedMessage), who hits
// whom for how much (ServerSoldierSoldierDamageMessage: victim soldier, damage giver, health before / after) and the
// kills (scoring::Kill). Logged once a minute per bot - shots, hits, accuracy, damage, hits on you, kills, headshot
// kills - plus every hit you land on a bot with its health before and after, to tune the bots' accuracy and to see
// why some bots soak several close-range shotgun blasts. Bot bullets are also traced (ServerCollisionProjectileFire:
// spawn point and velocity): how far each strays from the bot's aim (the weapon's spread), whether its path crosses
// the target's body, and whether the server then registers a hit - for bots shooting bots and bots shooting you.

#include <vector>

#include "brain.h"
#include "scoring.h"

namespace combatlog {

// Reads [combatlog] (and [bots] BotHitsOnBotsByServer) from mohw-bots.ini. The offline match runs as "coop", where
// the server drops its own hits from AI shooters (damageInfo flagged client-arbitrable) and waits for a player's game
// to report them: bot-vs-bot fights nobody was watching never did damage. With BotHitsOnBotsByServer the server
// applies a bot's hit on a bot itself, and players' reports of bot-vs-bot hits are ignored (no double damage). Bot
// hits on you stay as they were (your game reports them).
void Init();

// From the MessageManager::dispatch hook (any thread): records shots and damage.
void OnMessage(const void* message);

// Server thread, once a tick: attributes the recorded events (soldier -> player via `actors`), logs your hits and
// the per-minute summary. `nameOf` gives a bot's name (or null for players that aren't ours).
using NameFn = const char* (*)(void* player);
using KitFn = int (*)(void* player);
// A bot's current aim (authoritative yaw / pitch, radians) and the player it's fighting (null: none); false if not ours.
using AimFn = bool (*)(void* player, float* yaw, float* pitch, void** target);
void Tick(const std::vector<brain::Actor>& actors, const std::vector<scoring::Kill>& kills, NameFn nameOf, KitFn kitOf,
          AimFn aimOf);

}  // namespace combatlog
