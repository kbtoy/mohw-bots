#pragma once
// Battlefield 3's Hardcore preset on Warfighter's own settings ([hardcore] in mohw-bots.ini). BF3's table: friendly fire on,
// no health regeneration, 60% soldier health, no kill cam, HUD off, no name tags, no 3D spotting, spawn only on the squad
// leader; the minimap and minimap spotting stay on. The game reads all of these live (see README).

#include <cstdint>

namespace hardcore {

void Init();  // reads [hardcore]; lets the soldier-health setting apply to bots too, in any mode
bool Enabled();

// Turns it on or off now (ProCon's vars.preset): off puts back what the settings were before Hardcore changed them. Saved in
// mohw-bots.ini, so the next launch keeps it. False if it already was that way.
bool Set(bool on);

// Every 2 s, from ApplyOverrides: puts the rules back if a level load changed them and re-reads the two flags below.
// False while the settings don't exist yet.
bool Apply();

// True when Hardcore sets this setting (ForceMinimapSpotting leaves it alone then).
bool Owns(uintptr_t typeInfo, int offset);
// ProCon set this one by hand: Hardcore stops putting it back for the rest of this launch.
void Release(uintptr_t typeInfo, int offset);

// The game's settings as they are now, whoever set them (Hardcore or ProCon), at most 2 s old.
bool FriendlyFire();
bool LeaderSpawnOnly();

}  // namespace hardcore
