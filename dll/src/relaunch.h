#pragma once
// ProCon's Next Round and Restart Round (mapList.runNextRound / restartRound). Offline, each launch plays one level and
// the game never moves on by itself, so the DLL starts lanmohw.exe again (with no arguments, as you do) and closes this
// game; the new one's DllMain waits for the old one to be gone before it binds the RCON port and picks its map
// (mohw-bots-state.ini: NextMap, or Once for a restart). ProCon reconnects by itself.

namespace relaunch {

// DllMain, first thing: if this game was started by Now(), wait (up to 20 s) for the old one to close.
void WaitForPrevious();

// Starts the new game and ends this one. Returns only if the new one couldn't be started (logged; this game plays on).
bool Now();

}  // namespace relaunch
