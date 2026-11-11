#pragma once
// Stealth melee (the game's "humiliation melee"): the instant kill from your melee on a real player. Your game only
// accepts an AI soldier as the victim while it is in a canned-melee state the campaign sets up (0x981986, then
// 0x7EDBD0 on its ClientMeleeComponent), which a bot never is, so your melee falls through to the normal hit.
// [bots] StealthMeleeOnBots (default on) drops that condition; the server has no such check. The server runs the kill
// (ServerMeleeManager, the animation on both soldiers, then the damage, with you as the giver); a bot in one gets a
// neutral input, as the game's own AI brain does (0x1854F3B).

namespace melee {

void Init();                         // reads the setting, patches your game, hooks the server's start for the log
void ClientTick();                   // your game's thread, every 100 ms: logs the first bot the melee could target
bool Involved(void* serverSoldier);  // server thread: this soldier is in a stealth kill

}  // namespace melee
