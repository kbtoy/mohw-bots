#pragma once
// ProCon support: fills rconsrv (the RCON server) from the game and runs what ProCon asks for on the server thread.
// [rcon] in mohw-bots.ini: Enabled, Port, Bind, Password, ServerName.

#include <string>
#include <vector>

#include "scoring.h"

namespace rcon {

void Init();

// From the MessageManager::dispatch hook (any thread): chat.
void OnMessage(const void* message);

// Your game's thread, every tick: what ProCon said (admin.say) goes on your HUD chat as ADMIN.
void ClientTick();

// A line for ProCon's chat and your HUD (as admin.say shows). Any thread.
void Announce(const std::string& text);

// Server thread, every tick with a game context. A bot's name, or null for a player that isn't a bot.
using BotNameFn = const char* (*)(void* player);
void Tick(void* context, const std::vector<void*>& players, BotNameFn botName, const std::vector<scoring::Kill>& kills,
          bool roundOver, float dt);

}  // namespace rcon
