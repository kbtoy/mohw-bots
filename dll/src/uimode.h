#pragma once
// The game mode your game's HUD believes it is in.
//
// The in-game HUD graph (mohw/ui/flow/graph/frontend2/ig_hudgraph) shows each mode's HUD (tickets, flags, time) from
// "GET GAME MODE", which asks the UI settings component for data key 0x674C5F75; anything it doesn't recognise gets the
// Team Deathmatch HUD. The component's data provider (0xF57090) has the current mode under 0x674CE375 instead (the
// level setup's "gamemode" option, 0xF55DA0), so in this build the HUD's question was never answered and every mode
// showed the TDM HUD (2026-11-13: "no ticket count, progress bar" in Sector Control, launched straight into it too).
// This hooks the provider and answers both keys with the mode of the level the server has (in-place changes included).

#include <string>

namespace uimode {

void Install();                         // after eng::VerifySignatures()
void OnLevel(const std::string& key);   // server thread: the level key ("MP_21_SectorControl")

}  // namespace uimode
