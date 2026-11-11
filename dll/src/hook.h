#pragma once

#include <cstddef>

// Overwrites the first `stolen` bytes of target (which must be whole, position-independent instructions, >= 5 bytes)
// with a jmp to detour; returns a trampoline that runs the stolen bytes and continues, or null on failure.
void* InlineHook(void* target, size_t stolen, const void* detour);

// Inline-hooks Server::update (0xAA8DE0). The detour calls the original, then bots::OnServerUpdate on the same thread.
// Call only after eng::VerifySignatures() succeeded.
bool InstallServerUpdateHook();

// With an empty command line the game appends "-webmode spoffline" (offline mode, local server). Any real argument
// disables that and the game hands off to the EA app, so extra startup args are injected here instead: the append's
// string operand becomes "-webmode spoffline <extraArgs>". Must run before WinMain (i.e. from DllMain).
bool PatchStartupArgs(const char* extraArgs);

// Makes the Origin component behave as if VeniceOnlineSettings.EnableOrigin were false, so it never starts the
// Origin SDK (which launches the EA app and exits when an MP level is loaded offline). Breaks the LAN pack's
// emulated-Origin login, so only for offline testing.
bool PatchDisableOrigin();

// Server backend selection (0xD257B0) gives an MP level the configured online backend (Backend_Blaze), whose
// factory is only registered in online mode, so starting an MP level offline calls a null pointer (0xE9EA15).
// Forces every server onto Backend_Local (the campaign's backend). Breaks hosting through a real Blaze backend.
bool PatchForceLocalServerBackend();
// The server creates its animation manager (what moves soldiers' server-side hit shapes) in any game type, not only
// "multiplayer" (the offline match may run as "singleplayer"). See gen_signatures.py ServerAnim_*.
bool PatchForceServerAnimation();
// A bot's bullets do a player's damage: no campaign AI weapon multiplier ([bots] BotBulletsAsPlayers).
bool PatchBotBulletsAsPlayers();
bool PatchBotHealthFromServer();  // bots on your game follow the server's health, as a remote player's soldier does

// A connecting transport connection resends its challenge every 0.5 s and gives up after 15 tries (7.5 s), which
// the client peer reports as SecureReason_NoReply (16) and disconnects. The local server can't answer while it loads
// an MP level (~13 s), so this raises that limit (both challenge states, 0x61D6EA / 0x61D703) to `retries` (1..127).
bool PatchChallengeRetries(int retries);

// The startup "-level" path sends ClientStartMultiplayerMessage with both local flags clear, so the client reaches its
// own in-process server through a real UDP connection (mode 1), and the server's level load stalls until the client
// gives up. This makes the handler (0x8A69A1) treat msg+0x45 as set: connection mode 2, the in-process link the game
// itself uses for the front-end level. Breaks joining a remote server, so only for offline testing.
bool PatchLocalClientLink();
