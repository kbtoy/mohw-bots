#pragma once
// Offline scoring. In MOHW every score event - "+100 KILL", "+50 HEADSHOT", the fireteam buddy bonus - comes from the
// online stats object (0x2B14928): it measures stat events (kills, headshots, ...) against the ScoringTypeData list
// in mohw/persistence/scoring/scoring and dispatches the score message (0x6182A896) that the scoring system (round
// score, HUD popups, buddy bonus) and the support-action collection (the special-ability meter: 400 / 900 / 1500 /
// 2300 points) listen to. Offline that object doesn't exist, so nothing ever scored: 0 points, an empty meter.
// This sends the score messages for kills and headshots itself, with the game's own values.
// The objective modes score through their tokens: a TokenAction_GiveXP (a Combat Mission / Hot Spot charge armed or
// disarmed, the charge going off, a Home Run flag stolen or brought home) sends StatTokenPointsMessage (0xA9E33CD8:
// +0x28 player, +0x2C StatEvent, from 0xB72760) for the stats object to score. Those are scored here too - and only
// the score message moves the objective count on the end-of-round screen (PlayerScore.FlagsCaptured, +0x164: the
// scoring system's handler 0xE7F130 counts StatEvents 115, 173, 175, 190 and 202).

#include <vector>

namespace scoring {

// Captured when the kill is dispatched: without [bots] KeepPlayers a killed bot's player is destroyed right after
// (ReleasingLevel, Destroy).
struct Kill
{
    void* victim = nullptr;  // ServerPlayer (may be gone by the time the kill is processed)
    void* killer = nullptr;  // ServerPlayer (null: world, fall)
    bool headshot = false;
    bool victimIsAI = false;
    bool killerIsAI = false;
    int victimTeam = 0;
    int killerTeam = 0;
    char weapon[64] = {};  // the message's weapon name; empty if it had none
};

void Init();  // reads [scoring] from mohw-bots.ini
bool Enabled();
int KillPoints();      // [scoring] KillPoints (the game's 100)
int HeadshotPoints();  // [scoring] HeadshotPoints (the game's 50)

// From the MessageManager::dispatch hook (any thread): records ServerPlayerKilledMessages.
void OnMessage(const void* message);

// Server thread: the kills recorded since the last call.
std::vector<Kill> TakeKills();

// Server thread: sends the score events for a kill whose killer (still a valid player) is on the other team: a human,
// or (forHumanBuddy) a bot whose fireteam buddy is human - the game gives AI players no score, but their buddy gets
// the buddy bonus. Does nothing while the online stats object exists - it scores itself then.
void Award(const Kill& kill, bool forHumanBuddy);

// A stat event a mode's token handed a player (StatTokenPointsMessage).
struct TokenEvent
{
    void* player = nullptr;  // ServerPlayer
    int statEvent = 0;
};

// Server thread: the token events recorded since the last call.
std::vector<TokenEvent> TakeTokenEvents();

// What a token event is worth (the game's ScoringTypeData), or false if it has no score entry. `what` describes it
// for the log ("a charge armed").
bool TokenScore(int statEvent, int* points, const char** what);

// Server thread: sends the score message for a token event to a human player (still a valid player). Does nothing,
// returning false, while scoring is off or the online stats object exists.
bool AwardToken(const TokenEvent& event);

}  // namespace scoring
