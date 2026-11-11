#pragma once
// Scoreboard rows for bots. MOHW's scoring system keeps a record per player, and with it the replicated PlayerScore
// your scoreboard lists (name, kills, deaths, points). Its ServerPlayer listener (0xE80F40) creates a record only for
// a player that isn't AI (on ServerPlayerCreateMessage, +0x354 / +0x355 clear), counts a kill only for a killer that
// isn't AI (0xE7A6B0) and points only for a player with a connection (0xE7E4E0): bots had no row at all. This gives a
// bot a record when it joins its team (the listener's ServerPlayerSwitchTeamMessage), credits its kills and points
// itself, and removes the record when the bot's player goes (ServerPlayerDestroyMessage). Deaths count by the game's
// own path (0xE7E480), which has no AI check.

#include <vector>

namespace scoreboard {

void Init();  // reads [bots] Scoreboard, hooks the scoring system's ServerPlayer listener
bool Enabled();

// Server thread. A bot player that should get a record - call before its setTeam - and one that is gone.
void AddBot(void* player);
void ForgetBot(void* player);
// The server context changed: the listener seen before belongs to the old one.
void Reset();
// Server thread: a level is about to load in the same game (levelchange: Next / Restart Round in place). The context
// stays, so Reset doesn't run: forgets the points kept per row and sets the rows still there (yours) to 0.
void NewLevel();

// Server thread: a player killed an enemy. A bot: kills + 1 (the game never counts an AI killer's). Bots and you:
// the [scoring] points on the row - offline the game's own points stayed 0 (its per-class buckets never fill).
void CreditKill(void* killer, bool headshot);
// Server thread: points a player got for an objective (scoring::TokenScore), added to the points on its row.
void CreditPoints(void* player, int points);
// Server thread: the objective count on a player's row (PlayerScore.FlagsCaptured: charges armed / disarmed, flags
// captured) and its match total; false with no row.
bool ReadObjectives(void* player, int* count, int* total);

// Server thread, every tick: keeps each row's points at what CreditKill gave it, and the match totals the
// end-of-round screens show (best fireteam, results) equal to the round's numbers - the game adds them up only on
// ServerRoundSubRoundOverMessage, which TDM never sends.
void Tick(const std::vector<void*>& players, float dt);

// From the MessageManager::dispatch hook (any thread): a sub-round end hands the totals back to the game.
void OnMessage(const void* message);

// Client thread: gives your game's AI players their name in the online-name field the "killed by" card shows.
void ClientTick();

// Server thread, once a minute for the first twenty: kills / deaths / points on every row (yours and the bots').
using NameFn = const char* (*)(void* player);
// Server thread: a player's kills, deaths and points on its row; false (untouched) with no row.
bool ReadRow(void* player, int* kills, int* deaths, int* points);
void LogRows(const std::vector<void*>& players, NameFn nameOf, float dt);

// Client thread: what your game's scoreboard list holds - names and numbers per row (first twenty minutes).
void ClientDiagnostics();

}  // namespace scoreboard
