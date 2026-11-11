#pragma once
// Game-mode objectives the bots play: Sector Control's three flags.
//
// Where the flags are comes from the game's level data (objectives.inc, made by tools/objectives.py --sc-flags): the
// game's capture point entities (ServerCapturePointEntity) carry no position - their prefab's triggers decide who is
// inside. Who holds each comes from those entities (+0xA8 owner team), which a hook on their constructor and
// destructor keeps a list of. Which entity is which flag is learned in play: the first time an entity's capture
// progress moves, the one flag with soldiers standing at it is the one being taken. Until then a flag's owner counts as
// unknown (all neutral at the start, which is what the entities say anyway).

#include <string>
#include <vector>

#include "brain.h"

namespace objectives {

void Install();  // hooks the capture point constructor and destructor (every mode; logs what each level has)

// The level changed: key as nav's ("MP_03_SectorControl"). Picks up that map's flags when the mode has them.
void OnLevel(const std::string& key);

// Server thread, every tick: reads owners, learns which entity is which flag, logs captures, hands the bots the list.
void Tick(const std::vector<brain::Actor>& actors, float dt);

}  // namespace objectives
