#pragma once
// Bot brain: perception (line of sight), aiming, shooting and movement - a native port of the fun-bots infantry
// logic (refs/fun-bots/ext/Server/Bot, BotStates) onto MOHW's EntryInput. No waypoints yet: bots hunt the nearest
// enemy directly and get around obstacles with short ray probes.

#include <cstdint>
#include <vector>

#include "nav.h"

namespace brain {

// A living soldier, rebuilt every server tick by the caller.
struct Actor
{
    void* player = nullptr;
    void* soldier = nullptr;
    int team = 0;
    float pos[3] = {};  // feet
    bool isBot = false;
    float spawnAge = 99.0f;  // seconds since this soldier spawned
    int pose = 0;            // eng::Pose: stand, crouch, prone
    int state = 0;           // eng::CharacterState: on ground, swimming, sliding, ...
    float speed = 0.0f;      // horizontal metres per second (smoothed)
    float sinceFired = 99.0f;  // seconds since it last fired a shot
};

// An enemy a bot is in the middle of noticing: 0 = unnoticed, 1 = noticed (engages).
struct Awareness
{
    void* player = nullptr;
    float level = 0.0f;
};

struct State
{
    uint32_t rng = 0;
    bool aimInitialised = false;
    float yaw = 0.0f;    // current authoritative aiming (radians; 0 = +Z, increasing toward -X, as fun-bots)
    float pitch = 0.0f;  // positive = up

    // Target.
    void* target = nullptr;  // ServerPlayer
    float targetPos[3] = {};
    float sinceSeen = 99.0f;  // seconds since the last successful line-of-sight check on the target
    float reaction = 0.0f;    // seconds left before the bot may fire at a new target
    float visTimer = 0.0f;
    bool headAim = false;     // aiming at the head of the current target (rolled when it was acquired)
    float settle = 0.0f;      // 1 right after acquiring a target, decaying to 0: extra aim error while settling
    float sinceFired = 99.0f;  // seconds since the trigger was last pulled
    std::vector<Awareness> awareness;  // enemies seen but not noticed yet (or noticed a moment ago)
    float checkInterval = 0.2f;        // seconds between sight checks (the one running now)
    float lookYaw = 0.0f;              // turn toward a noise heard outside the field of view...
    float lookTimer = 0.0f;            // ...for this long
    float hearCooldown = 0.0f;

    // Posture: bots never crouch, go prone or swim on purpose.
    float offPoseTime = 0.0f;   // seconds crouched / prone in a row (stands up with jump)
    float swimTime = 0.0f;      // seconds swimming in a row (heads back to dry land)
    float dryPos[3] = {};       // the last place it stood on dry ground (checked once a second)
    bool hasDryPos = false;
    float dryTimer = 0.0f;
    // Getting out of water: the exit it swims for, progress toward it, and exits that didn't work (a wall or a
    // ledge too high - bots piled up in the bay behind the sniper tower pushing at one spot).
    float swimExit[3] = {};
    bool hasSwimExit = false;
    float swimCheckTimer = 0.0f;
    float swimCheckDist = 0.0f;
    float failedExits[4][3] = {};
    int failedExitCount = 0;

    // Firing.
    bool fireOn = false;
    float burstTimer = 0.0f;
    float triggerTime = 0.0f;  // seconds the trigger was held since the last reload
    float reloadHold = 0.0f;
    float aimErrTimer = 0.0f;
    float aimErrYaw = 0.0f;      // current sway
    float aimErrPitch = 0.0f;
    float swayTargetYaw = 0.0f;  // where the sway is drifting to
    float swayTargetPitch = 0.0f;
    float recoil = 0.0f;         // simulated muzzle climb (radians), grows while firing
    float grenadeCooldown = 0.0f;
    float grenadeHold = 0.0f;    // > 0: lining up / throwing a grenade
    bool grenadeRolled = false;  // already decided for the target's current disappearance

    // Movement.
    bool hasGoal = false;
    float goal[3] = {};
    float goalTimer = 0.0f;
    // Where it heads with no target in sight: its leaning (rolled once: hunter, flanker or roamer) picks between
    // hunting an enemy, swinging wide around one and roaming the map - so a team spreads out instead of walking at the
    // same nearest enemy as one pack.
    int style = -1;
    int goalKind = 0;  // GoalKind of the current goal
    int objective = -1;  // kGoalObjective: which one (index into the list SetObjectives gave)
    float probeTimer = 0.0f;
    float detourTimer = 0.0f;
    float detourYaw = 0.0f;
    float jumpHold = 0.0f;
    float stuckTimer = 0.0f;
    float progressPos[3] = {};
    float blindTimer = 0.0f;  // flashbanged: can't see, hear or shoot
    int flashSeen = -1;       // the last flashbang (grenades::Flash id) it has reacted to
    int stuckStreak = 0;  // checks (1.5 s each) that found it stuck since its spell began
    int spellChecks = 0;  // every check since then
    bool spellTrapMarked = false;  // this spell's spot is already a trap
    bool changeClass = false;      // CheckStuck's verdict: every way out failed, die and respawn
    float stuckPos[3] = {};  // where the spell began; it ends 5 m away
    float freePos[3] = {};  // where it was 1.5 s before its last check that found it moving
    bool hasFreePos = false;
    float attackMoveTimer = 0.0f;
    bool moveWhileShooting = true;
    float strafeTimer = 0.0f;
    float strafeDir = 1.0f;

    // Route over the learned navigation graph (empty: walk straight at the goal).
    std::vector<nav::Waypoint> path;
    size_t pathIndex = 0;
    float replanTimer = 0.0f;
    int jumpedAt = -1;  // path index whose jump link was already jumped

    // Diagnostics.
    bool loggedAimCheck = false;
    float aimCheckTimer = 0.0f;
    int stuckLogs = 0;
    int routeLogs = 0;
    int postureLogs = 0;
};

void Init();                 // reads [brain] from mohw-bots.ini
bool Enabled();
void SetEnabled(bool enabled);

// One server tick for one bot that has a soldier: decides and writes its EntryInput (movement, aim, fire, jump,
// sprint, reload). kitType = VeniceSoldierCustomizationAsset.MPKitType (4 = sniper, 6 = support). The bot's own
// entry in `actors` gives its pose and character state.
void Update(State& state, const char* name, void* player, void* soldier, void* input, int team, int kitType,
            const std::vector<Actor>& actors, float dt);

// Neutral input (brain disabled): stand still, don't fire.
void Idle(void* input);

// Fighting within the last `window` seconds (target in sight or shooting): teammates can't spawn on it.
bool InCombat(const State& state, float window);

// Goals picked since the last call, by kind (hunt, flank, roam, objective), for the log; resets the counts.
enum GoalKind { kGoalHunt, kGoalFlank, kGoalRoam, kGoalObjective, kGoalKinds };
void TakeGoalCounts(int counts[kGoalKinds]);

// A place the game mode wants taken or held (a Sector Control flag). owner: the team holding it, 0 nobody, -1 not
// known yet (treated as not ours). Bots with no target head for ones their team doesn't hold, or hold their own when an
// enemy is near; once there they stay inside `radius` until it's theirs.
struct Objective
{
    float pos[3] = {};
    float radius = 4.0f;
    int owner = -1;
    char letter = '?';
};
// The current list (empty: the mode has none, bots play as in Team Deathmatch). Server thread, before Update.
void SetObjectives(const std::vector<Objective>& objectives);

}  // namespace brain
