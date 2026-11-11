// Runs dll/src/rconsrv.cpp with made-up data, no game: point a copy of ProCon at 127.0.0.1:<port>.
// rcontest [port] [password] [log file]. Prints each command the "game" would run, and sends a kill, a spawn, chat, a team
// change, a leave, a join and a round end on a timer, so ProCon's parsing of every event gets used.
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../dll/src/rconsrv.h"

using namespace rconsrv;

static FILE* g_log = stdout;

static void LogLine(const char* text)
{
    std::fprintf(g_log, "%s\n", text);
    std::fflush(g_log);
}

// A stand-in for the DLL's check (modes.cpp): the four tested maps, Novi Grad without Home Run, Basilan without Hot Spot.
static const char* CheckMap(std::string& map, std::string& mode)
{
    const char* maps[] = {"MP_03", "MP_05", "MP_10", "MP_12"};
    const char* modes[] = {"TeamDeathMatch", "CombatMission", "SectorControl", "BombSquad", "Sport"};
    bool knownMap = false, knownMode = false;
    for (const char* m : maps)
        if (_stricmp(m, map.c_str()) == 0)
        {
            map = m;
            knownMap = true;
        }
    for (const char* m : modes)
        if (_stricmp(m, mode.c_str()) == 0)
        {
            mode = m;
            knownMode = true;
        }
    if (!knownMap)
        return "InvalidMap";
    if (!knownMode || (map == "MP_05" && mode == "Sport") || (map == "MP_12" && mode == "BombSquad"))
        return "InvalidGameModeOnMap";
    return nullptr;
}

int main(int argc, char** argv)
{
    Config config;
    config.port = argc > 1 ? std::atoi(argv[1]) : 47201;
    if (argc > 2 && std::string(argv[2]) != "-")
        config.password = argv[2];
    if (argc > 3)
        g_log = std::fopen(argv[3], "w");
    config.log = &LogLine;
    config.checkMap = &CheckMap;
    if (!Start(config))
        return 1;

    Snapshot s;
    s.map = "MP_03";
    s.mode = "TeamDeathMatch";
    s.teamScores[0] = 12;
    s.teamScores[1] = 9;
    s.targetScore = 150;
    SetMapList({{"MP_05", "TeamDeathMatch", 1}, {"MP_03", "TeamDeathMatch", 1}, {"MP_10", "SectorControl", 1}}, 2);
    for (int i = 0; i < 20; ++i)
    {
        Player p;
        p.name = i == 0 ? "TestPlayer" : "Bot" + std::to_string(i);
        p.guid = "EA_" + std::string(30, 'A') + std::to_string(10 + i);
        p.team = i % 2 + 1;
        p.squad = i / 4 + 1;
        p.kills = i;
        p.deaths = 20 - i;
        p.score = i * 100;
        p.bot = i != 0;
        s.players.push_back(p);
    }
    // No level yet: what ProCon sees if it connects while the game is still starting.
    Snapshot empty;
    empty.mode = s.mode;
    Publish(empty);
    Sleep(argc > 4 ? 6000 : 0);
    Publish(s);
    for (const char* name : {"vars.serverName", "vars.serverDescription", "vars.serverMessage", "vars.bannerUrl", "vars.gamePassword"})
        SetVar(name, "", false);
    SetVar("vars.preset", "normal", true);
    // RCONTEST_OLD=1: act like a build from before vars.hardCore (2026.11.07.4 and older).
    const bool old = std::getenv("RCONTEST_OLD") != nullptr;
    if (!old)
        SetVar("vars.hardCore", "false", true);
    for (const char* name : {"vars.autoBalance", "vars.friendlyFire", "vars.regenerateHealth", "vars.killCam", "vars.3pCam", "vars.ranked",
                             "vars.hudMinimap", "vars.hudCrosshair", "vars.buddyOutline", "vars.hudBuddyInfo", "vars.hudClassAbility",
                             "vars.hudEnemyTag", "vars.hudExplosiveIcons", "vars.hudGameMode", "vars.hudHealthAmmo", "vars.hudObiturary",
                             "vars.hudPointsTracker", "vars.hudUnlocks"})
        SetVar(name, "true", true);
    for (const char* name : {"vars.maxPlayers", "vars.bulletDamage", "vars.gameModeCounter", "vars.soldierHealth", "vars.playerRespawnTime",
                             "vars.playerManDownTime", "vars.idleTimeout", "vars.idleBanRounds", "vars.roundStartPlayerCount",
                             "vars.roundRestartPlayerCount", "vars.teamKillCountForKick", "vars.teamKillValueForKick",
                             "vars.teamKillValueIncrease", "vars.teamKillValueDecreasePerSecond"})
        SetVar(name, "100", true);

    for (int second = 1;; ++second)
    {
        Sleep(1000);
        s.roundSeconds = second;
        s.uptimeSeconds = second;
        Publish(s);
        for (const Words& command : TakeCommands())
        {
            std::string line = "command:";
            for (const std::string& word : command)
                line += " [" + word + "]";
            LogLine(line.c_str());
            if (command.size() > 1 && (command[0] == "vars.preset" || command[0] == "vars.hardCore"))
            {
                const bool on = command[1] == "hardcore" || command[1] == "true";
                s.serverName = on ? "mohw-bots [Hardcore]" : "mohw-bots";
                SetVar("vars.preset", on ? "hardcore" : "normal", true);
                if (!old)
                    SetVar("vars.hardCore", on ? "true" : "false", true);
                SetVar("vars.killCam", on ? "false" : "true", true);
            }
        }
        switch (second)
        {
        case 8:
            Event({"server.onLevelLoaded", "MP_03", "TeamDeathMatch", "0", "1"});
            Event({"player.onJoin", "Joiner", "EA_JOINER"});
            Event({"player.onAuthenticated", "Joiner", "EA_JOINER"});
            break;
        case 10:
            Event({"player.onSpawn", "Bot1", "2"});
            Event({"player.onKill", "TestPlayer", "Bot1", "MOHW/Weapons/MP_Weapons/MK18_MP", "true"});
            Event({"player.onKill", "Bot2", "TestPlayer", "Death", "false"});
            Event({"player.onKill", "", "Bot5", "Death", "false"});
            Event({"player.onChat", "TestPlayer", "hello from the test"});
            Event({"player.onTeamChange", "Bot3", "1", "0"});
            Event({"player.onSquadChange", "Bot3", "1", "2"});
            break;
        case 14:
        {
            Words leave = {"player.onLeave", "Bot4"};
            leave.insert(leave.end(), {"7", "name", "guid", "teamId", "squadId", "kills", "deaths", "score", "1", "Bot4", "EA_B4", "1", "0", "1", "2", "100"});
            Event(leave);
            break;
        }
        case 18:
        {
            Words players = {"server.onRoundOverPlayers"};
            players.insert(players.end(), {"7", "name", "guid", "teamId", "squadId", "kills", "deaths", "score", "1", "TestPlayer", "EA_K", "1", "0", "5", "1", "500"});
            Event(players);
            Event({"server.onRoundOverTeamScores", "2", "150", "80", "150"});
            Event({"server.onRoundOver", "1"});
            break;
        }
        default:
            break;
        }
    }
}
