#pragma once
// Navigation graph learned from human movement. MOHW's MP levels ship no navmesh, so - like fun-bots' recorded paths,
// but automatic - every human's walk drops nodes every ~2.5 m and links consecutive ones; bots plan routes over the
// graph with A*. One file per level and game mode in <dll dir>\mohw-bots-nav\, loaded on level load, saved while it
// grows.

#include <cstdint>
#include <string>
#include <vector>

namespace nav {

struct Waypoint
{
    float pos[3] = {};
    bool jump = false;  // the link into this waypoint climbed (the human jumped); press jump on the way
    int node = -1;
};

void Init();  // reads [nav] from mohw-bots.ini

// The level changed (or first seen): saves the previous graph, loads this one. key = "MP_03_TeamDeathMatch" style.
void OnLevel(const std::string& key);

// Per server tick, for every living human player (and bot, if LearnFromBots): grows the graph along their walk.
// player identifies the walker. A bot's walk is as walkable as a human's, so bots that find their way out of a
// room teach the others the exit.
void Learn(void* player, const float feet[3], float dt);
bool LearnFromBots();
void ForgetWalker(void* player);  // died / left

// The bots learn this map ("MP_03") again from scratch: its files become <name>.nav.reset-<time>.bak, and if it's the
// level being played its graph empties now. Server thread. Returns how many files were set aside; *wasCurrent says
// whether it was the level being played.
int ResetMap(const std::string& map, bool* wasCurrent);

// Saves when changed, at most every SaveSeconds. Call once per server tick.
void Tick(float dt);

size_t NodeCount();

// A* route from `from` to `to` (feet positions): the node waypoints in order (empty if either end has no node within
// reach or they aren't connected). The caller walks the last stretch to `to` itself.
// failure (optional): 1 = no node near `from`, 2 = no node near `to`, 3 = both have nodes but aren't connected.
bool FindPath(const float from[3], const float to[3], std::vector<Waypoint>& out, int* failure = nullptr);

// A bot got stuck walking the link a -> b: make it more expensive so routes avoid it.
void Penalize(int a, int b);
// A bot stayed stuck at `pos` (a nook it walked into and couldn't walk out of): every link into the nodes within
// 1.5 m (and 1.5 m of height) gets the highest penalty, so routes only go there when nothing else does. Those nodes
// become trap spots: never a route's goal or a node handed out below, and links learned into them later get the same
// penalty. Returns how many links that raised.
int MarkTrap(const float pos[3]);

// The nearest dry node (not a trap) within `radius` metres (any height within 10 m): somewhere to swim back to.
bool NearestNode(const float p[3], float radius, float out[3]);
// The nearest dry node within `radius` that is at least `minSep` metres from every point in `avoid` (exits a
// swimming bot already failed to climb out at) and at most `maxRise` metres above `p` (a bank it can climb).
bool NearestNodeAvoiding(const float p[3], float radius, const float (*avoid)[3], int avoidCount, float minSep,
                         float out[3], float maxRise = 10.0f);
// A random dry node `minDist`..`maxDist` metres (flat) from `p`, picked with `seed`: somewhere to roam to. False when
// none of the nodes tried is in that band.
bool RandomNode(uint32_t seed, const float p[3], float minDist, float maxDist, float out[3]);

// Probes the next `count` nodes (round robin) with `depthAt` (water depth; server thread) and marks those deeper than
// `maxDepth` wet: routes, nearest-node lookups and learning skip them (bots that swam once taught the graph water
// nodes). Returns how many became wet.
int ScanForWater(float (*depthAt)(const float pos[3]), int count, float maxDepth);

}  // namespace nav
