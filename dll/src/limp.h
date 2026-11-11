#pragma once
// Limp bodies: a bot your game shows dead while the server has it alive and fighting. It moves about on your screen as
// a ragdoll. Your game used to run its own estimate of a bot's health: the server's plus the hits it saw itself, which
// the server never acknowledged (since the fourteenth round it ignores claims of bots hitting bots).
// [bots] BotHealthFromServer (hook.cpp) removes the cause: your game follows the server's health for bots. This file
// then only watches: it logs if your game's estimate still reaches 0 with the server alive, and if your game's copy of
// the server's health stops following the server (what the fix relies on).
// With BotHealthFromServer off, it catches them instead: once your game has shown a bot dead for [bots] LimpBodyKillMs
// (500) while the server's soldier still has health, the server kills it too.

namespace limp {

void Init();        // reads [bots] LimpBodyKillMs (0 = off) and BotHealthFromServer
void ClientTick();  // your game's thread, every 100 ms: reads each AI player's soldier health there

// True when BotHealthFromServer is on: ShownDeadOnlyThere's answer is for the log only, nothing to kill.
bool WatchOnly();

// Server thread, for a bot with a living soldier: true when your game has shown this player's soldier dead for
// LimpBodyKillMs while the server's has health (the caller kills it); outShownMs / outServerHealth for the log.
bool ShownDeadOnlyThere(void* serverPlayer, void* serverSoldier, unsigned* outShownMs, float* outServerHealth);

// Server thread, for a bot with a living soldier, with BotHealthFromServer on: logs the first time your game's copy of
// the server's health is seen following a hit, and any bot whose copy stays off the server's for 1 s.
void CheckCopy(void* serverPlayer, void* serverSoldier, const char* name);

}  // namespace limp
