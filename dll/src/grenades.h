#pragma once
// Flashbangs and smoke for the bots. Every server explosion is announced by ServerCollisionExplosionSpawnMessage (built
// by the explosion entity's detonate, 0xBDD840): +0x28 the ServerExplosionEntity (its +0x14 data, an
// ExplosionEntityData), +0x2C the thrower's player, +0x30 the position, +0x40 / +0x44 the scaled blast radius /
// damage. A flashbang's data has HasStunEffect (+0xC4). Smoke is read live from the server's own smoke screens (every
// ServerSmokeScreenEntity keeps itself in a global list; its SmokeScreenEntityData gives the radius, lifetime and
// shrink): no smoke grenade ever showed up among the explosions. Your game draws the white-out and the smoke for you;
// bots get them here - a flashbang blinds and deafens the bots that see it, smoke blocks their line of sight.

#include <cstdint>
#include <vector>

namespace grenades {

struct Flash
{
    int id = 0;
    float pos[3] = {};
    uint64_t at = 0;  // GetTickCount64 ms
};

void OnMessage(const void* message);  // from the message hook

// Flashbangs from the last 2 s, oldest first; ids increase. LastFlashId() = the newest id so far (0 = none).
std::vector<Flash> RecentFlashes();
int LastFlashId();

// Does the line from `from` to `to` pass through smoke that's up now? Server thread.
bool SmokeBlocks(const float from[3], const float to[3]);
// Once per server tick: logs each smoke screen as it comes up, and as it clears with how many bot sight lines it hid.
void ServerTick();

}  // namespace grenades
