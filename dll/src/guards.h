#pragma once

// Offline the online stats object at 0x2B14928 is never created. Most code checks it for null; three places don't
// and crash the game the first time they run (the first was a kill: suicide-to-respawn crashed at 0xE75637). These
// guards make those places take the game's own "no stats" path when the object is missing; with the object present
// they change nothing. Also guards the client death screen, which crashed reading the persistence record of a bot
// that killed you (bots have none). Call only after eng::VerifySignatures() succeeded.
bool InstallOfflineStatsGuards();
