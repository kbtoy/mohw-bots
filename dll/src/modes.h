#pragma once
// Game modes and the map rotation. A mode is a sub-level the level includes by the GameSettings.DefaultLayerInclusion
// criterion GameMode (README "Game modes"). The names are the game's own criterion values, which are also what
// ProCon's MOHW.def uses: TeamDeathMatch, CombatMission, SectorControl, BombSquad (Hot Spot), Sport (Home Run).

#include <string>
#include <vector>

namespace modes {

struct Entry
{
    std::string map;   // "MP_03"
    std::string mode;  // "SectorControl"
};

// The mode's name as the game shows it ("Sector Control"), or the mode itself if unknown.
const char* DisplayName(const std::string& mode);
// The map's name ("Somalia Stronghold"), or nullptr if unknown.
const char* MapName(const std::string& map);
// The exact spelling of a known mode, matched without regard to case ("sectorcontrol" -> "SectorControl"); empty if
// it isn't one.
std::string Canonical(const std::string& mode);
// The map ships a sub-level for this mode (from each map's .toc; the same list as ProCon's MOHW.def).
bool MapHasMode(const std::string& map, const std::string& mode);
// The maps' and modes' canonical spellings, or empty if `map` isn't a known map code.
std::string CanonicalMap(const std::string& map);

// One entry, "MP_03" or "MP_03:SectorControl" (any case); fallbackMode when it names none. False for an unknown map; a
// mode the map doesn't have becomes Team Deathmatch. Both logged, with `where` saying which setting it came from.
bool ParseEntry(const std::string& text, const std::string& fallbackMode, const char* where, Entry* out);

// [launch] Map: "MP_05,MP_03:SectorControl,..." - an entry without ":Mode" plays [launch] Mode (Team Deathmatch by
// default). Entries for unknown maps are dropped; a mode a map doesn't have becomes Team Deathmatch (logged).
std::vector<Entry> ReadRotation(const wchar_t* ini);
// Writes the rotation back in the same form (an entry whose mode is [launch] Mode is written without it).
bool WriteRotation(const wchar_t* ini, const std::vector<Entry>& rotation);
std::string FormatRotation(const std::vector<Entry>& rotation, const std::string& defaultMode);

// What is being played: set at startup, and again by an in-place level change (levelchange.cpp).
void SetCurrent(const std::string& map, const std::string& mode);
const std::string& CurrentMode();
const std::string& CurrentMap();
bool IsTeamDeathmatch();  // the mode the bots were written for: own-base spawns, camped bases, the kill target
bool OneLifePerRound();   // Home Run: nobody respawns until the next round

}  // namespace modes
