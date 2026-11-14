#pragma once
// The game mode your game's HUD believes it is in.
//
// The in-game HUD graph (mohw/ui/flow/graph/frontend2/ig_hudgraph) shows each mode's HUD from "GET GAME MODE", which asks
// the UI settings component for data key 0x674CE375 (1733092213); its provider (0xF57090) answers with the "gamemode"
// option of a level setup (0xF55DA0), the one your game was started with. This hooks the provider and answers with the
// mode of the level the server has, so after an in-place change (TDM -> Sector Control) the HUD should follow.
// (2026.11.13.3 took the key for 0x674C5F75 - a hex slip - and called it a mismatch; there is none, and on a launch
// straight into a mode the game already answered right. The missing Sector Control HUD was Hardcore's HudNoGameMode.)

#include <string>

namespace uimode {

void Install();                         // after eng::VerifySignatures()
void OnLevel(const std::string& key);   // server thread: the level key ("MP_21_SectorControl")

}  // namespace uimode
