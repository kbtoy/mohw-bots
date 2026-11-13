#pragma once
// Game-mode objectives the bots play: Sector Control's three flags.
//
// Where the flags are comes from the game's level data (objectives.inc, made by tools/objectives.py --sc-flags): the
// game's capture point entities (ServerCapturePointEntity) carry no position. Which entity is which flag comes from
// its data's FlagID. Nothing in this build runs their capture, so Tick does (counts who is inside each flag and calls the
// game's capture step; see DriveCaptures). Who holds each comes from those entities (+0xA8 owner team), which a hook on their constructor and
// destructor keeps a list of. Which entity is which flag is learned in play: the first time an entity's capture
// progress moves, the one flag with soldiers standing at it is the one being taken. Until then a flag's owner counts as
// unknown (all neutral at the start, which is what the entities say anyway).

#include <string>
#include <vector>

#include "brain.h"

namespace objectives {

// Hooks the capture point constructor and destructor (every mode; logs what each level has).
void Install();

// The level changed: key as nav's ("MP_03_SectorControl"). Picks up that map's flags when the mode has them.
void OnLevel(const std::string& key);

// Server thread, every tick: reads owners, learns which entity is which flag, logs captures, hands the bots the list.
void Tick(const std::vector<brain::Actor>& actors, float dt);

// Sector Control with flags: the flag nearest `p` (flat), its letter, place and holder (1 / 2, 0 nobody, -1 not linked).
// False in other modes. Server thread.
bool NearestFlag(const float p[3], char* letter, float pos[3], int* owner);
bool HasFlags();  // Sector Control with its flags known and the capture point hooks in

}  // namespace objectives
