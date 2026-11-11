#pragma once
// A small Frostbite RCON server, what ProCon talks to. lanmohw.exe keeps the settings for remote administration but none
// of the command handlers (they were in the separate dedicated server exe), so this answers instead. No game code in
// here: rcon.cpp fills it from the game, and tools/smoketest/rcontest.cpp runs it with made-up data against ProCon.
//
// Wire format: u32 header (bit 31 = the exchange started on the server, bit 30 = this is the response, the rest a
// sequence number), u32 total size, u32 word count, then per word u32 length, the bytes and a NUL.

#include <string>
#include <vector>

namespace rconsrv {

using Words = std::vector<std::string>;

struct Player
{
    std::string name;
    std::string guid;
    int team = 0;
    int squad = 0;  // 0 = none
    int kills = 0;
    int deaths = 0;
    int score = 0;
    bool bot = false;
};

struct MapEntry
{
    std::string map;   // "MP_03"
    std::string mode;  // "TeamDeathMatch"
    int rounds = 1;
};

struct Snapshot
{
    std::string serverName = "mohw-bots";
    std::string map;
    std::string mode;
    int maxPlayers = 20;
    int teamScores[2] = {0, 0};
    int targetScore = 0;
    int roundSeconds = 0;
    int uptimeSeconds = 0;
    std::vector<Player> players;
    // The rotation and its indices are the server's own once SetMapList has run: ProCon edits them, Publish leaves them.
    std::vector<MapEntry> maps;
    int mapIndex = 0;
    int nextMapIndex = 0;
};

struct Config
{
    int port = 47200;
    std::string bind = "127.0.0.1";
    std::string password;  // empty: any login is accepted (fine on 127.0.0.1)
    std::string build = "323544";  // the last server build ProCon's MOHW.def knows (R-10)
    void (*log)(const char* text) = nullptr;
    // mapList.add: puts map and mode in their canonical spelling and returns null if the map has that mode, else the
    // error word to answer ("InvalidMap", "InvalidGameModeOnMap"). Null function: anything goes.
    const char* (*checkMap)(std::string& map, std::string& mode) = nullptr;
};

// Starts the listening thread. False if it couldn't start (the reason is logged).
bool Start(const Config& config);

// Any thread. The data the read commands answer from (not the rotation, see SetMapList).
void Publish(const Snapshot& snapshot);

// Any thread, once at start: the rotation ([launch] Map) and the entry the next launch plays. From then on ProCon's
// mapList.add / remove / clear / setNextMapIndex change it here and queue {"mapList.persist", <next index>, <map>,
// <mode>, ...} with the whole list for the game thread to save.
void SetMapList(const std::vector<MapEntry>& maps, int nextMapIndex);
// Any thread: the rotation moved on (an in-place level change): the entry the next Next Round plays. Not queued to save.
void SetNextMapIndex(int nextMapIndex);

// Any thread. A vars.* value; writable ones are accepted and queued as a command, the others answer CommandIsReadOnly.
void SetVar(const std::string& name, const std::string& value, bool writable);

// Any thread. An event to every connection that asked for events (admin.eventsEnabled true).
void Event(const Words& words);

// Game thread. Commands that change the game, in the order received: validated and answered already.
std::vector<Words> TakeCommands();

}  // namespace rconsrv
