#include "rconsrv.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

namespace rconsrv {
namespace {

constexpr uint32_t kFromServer = 0x80000000u;
constexpr uint32_t kResponse = 0x40000000u;
constexpr uint32_t kSequenceMask = 0x3FFFFFFFu;
constexpr uint32_t kMaxPacket = 16384;  // a Frostbite server's limit; ProCon stays well under it

struct Var
{
    std::string value;
    bool writable = false;
};

struct Connection
{
    SOCKET socket = INVALID_SOCKET;
    std::string in;
    bool loggedIn = false;
    bool events = false;
    bool closing = false;
    unsigned char salt[16] = {};
};

struct Lock
{
    CRITICAL_SECTION section;
    Lock() { InitializeCriticalSection(&section); }
};
Lock g_lockObject;  // from the DLL's static start-up: SetVar and Publish can come before Start
CRITICAL_SECTION& g_lock = g_lockObject.section;
Config g_config;
Snapshot g_snapshot;
std::map<std::string, Var> g_vars;
std::vector<Words> g_events;
std::vector<Words> g_commands;
uint32_t g_eventSequence = 0;
// ProCon's Maplist tab adds maps of the server's "playlist" only (its MOHW pool is grouped by mode), and changes it with
// admin.setPlaylist. Here it's just the mode ProCon adds maps with: nothing in the game changes.
std::string g_playlist;

class Locked
{
public:
    Locked() { EnterCriticalSection(&g_lock); }
    ~Locked() { LeaveCriticalSection(&g_lock); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
};

void Say(const char* format, const char* a = "", int b = 0)
{
    if (!g_config.log)
        return;
    char text[256];
    std::snprintf(text, sizeof(text), format, a, b);
    g_config.log(text);
}

std::string Encode(uint32_t header, const Words& words)
{
    std::string body;
    for (const std::string& word : words)
    {
        uint32_t length = static_cast<uint32_t>(word.size());
        body.append(reinterpret_cast<const char*>(&length), 4);
        body += word;
        body.push_back('\0');
    }
    uint32_t count = static_cast<uint32_t>(words.size());
    uint32_t size = 12 + static_cast<uint32_t>(body.size());
    std::string packet;
    packet.append(reinterpret_cast<const char*>(&header), 4);
    packet.append(reinterpret_cast<const char*>(&size), 4);
    packet.append(reinterpret_cast<const char*>(&count), 4);
    packet += body;
    return packet;
}

bool Decode(const char* data, uint32_t size, Words* words)
{
    if (size < 12)
        return false;
    uint32_t count = 0;
    std::memcpy(&count, data + 8, 4);
    uint32_t at = 12;
    words->clear();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (at + 4 > size)
            return false;
        uint32_t length = 0;
        std::memcpy(&length, data + at, 4);
        at += 4;
        if (length > size || at + length + 1 > size)
            return false;
        words->emplace_back(data + at, length);
        at += length + 1;
    }
    return true;
}

void Send(Connection& c, const std::string& packet)
{
    size_t sent = 0;
    while (sent < packet.size())
    {
        int n = send(c.socket, packet.data() + sent, static_cast<int>(packet.size() - sent), 0);
        if (n == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
            {
                fd_set out;
                FD_ZERO(&out);
                FD_SET(c.socket, &out);
                timeval wait = {1, 0};
                if (select(0, nullptr, &out, nullptr, &wait) > 0)
                    continue;
            }
            c.closing = true;
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

std::string Hex(const unsigned char* bytes, size_t count)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string text;
    for (size_t i = 0; i < count; ++i)
    {
        text.push_back(digits[bytes[i] >> 4]);
        text.push_back(digits[bytes[i] & 15]);
    }
    return text;
}

// MD5(salt + password), in capitals: what login.hashed sends back.
std::string HashedPassword(const unsigned char* salt, size_t saltSize, const std::string& password)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    std::string result;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        if (CryptCreateHash(provider, CALG_MD5, 0, 0, &hash))
        {
            unsigned char digest[16] = {};
            DWORD size = sizeof(digest);
            if (CryptHashData(hash, salt, static_cast<DWORD>(saltSize), 0) &&
                CryptHashData(hash, reinterpret_cast<const BYTE*>(password.data()), static_cast<DWORD>(password.size()), 0) &&
                CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0))
                result = Hex(digest, sizeof(digest));
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(provider, 0);
    }
    return result;
}

void NewSalt(unsigned char* salt, size_t size)
{
    HCRYPTPROV provider = 0;
    bool ok = false;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        ok = CryptGenRandom(provider, static_cast<DWORD>(size), salt) != FALSE;
        CryptReleaseContext(provider, 0);
    }
    if (!ok)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        for (size_t i = 0; i < size; ++i)
            salt[i] = static_cast<unsigned char>(now.QuadPart >> (i % 8 * 8));
    }
}

const char* Bool(bool value) { return value ? "true" : "false"; }

// <field count> <field names> <player count> <each player's fields>: the block admin.listPlayers, player.onLeave and
// server.onRoundOverPlayers share.
void AppendPlayers(Words& words, const std::vector<const Player*>& players)
{
    static const char* const fields[] = {"name", "guid", "teamId", "squadId", "kills", "deaths", "score"};
    words.push_back("7");
    for (const char* field : fields)
        words.push_back(field);
    words.push_back(std::to_string(players.size()));
    for (const Player* p : players)
    {
        words.push_back(p->name);
        words.push_back(p->guid);
        words.push_back(std::to_string(p->team));
        words.push_back(std::to_string(p->squad));
        words.push_back(std::to_string(p->kills));
        words.push_back(std::to_string(p->deaths));
        words.push_back(std::to_string(p->score));
    }
}

const Player* FindPlayer(const std::string& name)
{
    for (const Player& p : g_snapshot.players)
        if (p.name == name)
            return &p;
    return nullptr;
}

Words ServerInfo()
{
    const Snapshot& s = g_snapshot;
    Words w = {"OK", s.serverName, std::to_string(s.players.size()), std::to_string(s.maxPlayers), s.mode, s.map, "0", "1",
               "2", std::to_string(s.teamScores[0]), std::to_string(s.teamScores[1]), std::to_string(s.targetScore),
               s.map.empty() ? "NotConnected" : "AcceptingPlayers",
               "false", "false", "false",
               std::to_string(s.uptimeSeconds), std::to_string(s.roundSeconds),
               g_config.bind + ":" + std::to_string(g_config.port), "", "false", "local", "local", "", "false"};
    return w;
}

Words ListPlayers(const Words& request)
{
    std::vector<const Player*> rows;
    const std::string subset = request.size() > 1 ? request[1] : "all";
    for (const Player& p : g_snapshot.players)
    {
        bool in = subset == "all";
        if (subset == "team" && request.size() > 2)
            in = std::to_string(p.team) == request[2];
        else if (subset == "squad" && request.size() > 3)
            in = std::to_string(p.team) == request[2] && std::to_string(p.squad) == request[3];
        else if (subset == "player" && request.size() > 2)
            in = p.name == request[2];
        if (in)
            rows.push_back(&p);
    }
    Words w = {"OK"};
    AppendPlayers(w, rows);
    return w;
}

Words MapList()
{
    // ProCon's MOHW parser (MOHWClient::DispatchMapListListResponse) takes <map count> <dropped> <words per map>.
    Words w = {"OK", std::to_string(g_snapshot.maps.size()), std::to_string(g_snapshot.maps.size()), "3"};
    for (const MapEntry& m : g_snapshot.maps)
    {
        w.push_back(m.map);
        w.push_back(m.mode);
        w.push_back(std::to_string(m.rounds));
    }
    return w;
}

int CurrentIndex();

// The whole rotation for the game thread to save (mohw-bots.ini [launch] Map, mohw-bots-state.ini NextMap).
void QueuePersist()
{
    g_snapshot.mapIndex = CurrentIndex();  // the list changed under it
    Words w = {"mapList.persist", std::to_string(g_snapshot.nextMapIndex)};
    for (const MapEntry& m : g_snapshot.maps)
    {
        w.push_back(m.map);
        w.push_back(m.mode);
    }
    g_commands.push_back(w);
}

// The entry being played: the first that matches the map and mode (0 if the list doesn't have it).
int CurrentIndex()
{
    for (size_t i = 0; i < g_snapshot.maps.size(); ++i)
        if (g_snapshot.maps[i].map == g_snapshot.map && g_snapshot.maps[i].mode == g_snapshot.mode)
            return static_cast<int>(i);
    return 0;
}

bool ParseIndex(const std::string& text, int limit, int* out)
{
    if (text.empty() || text.size() > 6 || text.find_first_not_of("0123456789") != std::string::npos)
        return false;
    *out = std::atoi(text.c_str());
    return *out >= 0 && *out < limit;
}

Words MapListCommand(const Words& w)
{
    const std::string& command = w[0];
    std::vector<MapEntry>& maps = g_snapshot.maps;
    int& next = g_snapshot.nextMapIndex;
    if (command == "mapList.list")
        return MapList();
    if (command == "mapList.getMapIndices")
        return {"OK", std::to_string(g_snapshot.mapIndex), std::to_string(next)};
    if (command == "mapList.getRounds")
        return {"OK", "0", "1"};
    if (command == "mapList.setNextMapIndex")
    {
        int index = 0;
        if (w.size() != 2 || !ParseIndex(w[1], static_cast<int>(maps.size()), &index))
            return {"InvalidMapIndex"};
        next = index;
        QueuePersist();
        return {"OK"};
    }
    if (command == "mapList.add")
    {
        // mapList.add <map> <mode> <rounds> [<index>] (ProCon's MOHW client always sends rounds; it's one round a launch)
        if (w.size() < 3 || w.size() > 5)
            return {"InvalidArguments"};
        MapEntry entry;
        entry.map = w[1];
        entry.mode = w[2];
        if (g_config.checkMap)
            if (const char* error = g_config.checkMap(entry.map, entry.mode))
                return {error};
        int at = static_cast<int>(maps.size());
        if (w.size() == 5 && !ParseIndex(w[4], static_cast<int>(maps.size()) + 1, &at))
            return {"InvalidMapIndex"};
        if (maps.size() >= 32)
            return {"MapListFull"};
        const bool wasEmpty = maps.empty();
        maps.insert(maps.begin() + at, entry);
        if (wasEmpty)
            next = 0;
        else if (at <= next)
            ++next;  // it still points at the same entry
        QueuePersist();
        return {"OK"};
    }
    if (command == "mapList.remove")
    {
        int index = 0;
        if (w.size() != 2 || !ParseIndex(w[1], static_cast<int>(maps.size()), &index))
            return {"InvalidMapIndex"};
        maps.erase(maps.begin() + index);
        if (index < next)
            --next;
        if (next >= static_cast<int>(maps.size()))
            next = 0;
        QueuePersist();
        return {"OK"};
    }
    if (command == "mapList.clear")
    {
        maps.clear();
        next = 0;
        QueuePersist();
        return {"OK"};
    }
    if (command == "mapList.save")
    {
        QueuePersist();  // each change is saved already; again for good measure
        return {"OK"};
    }
    if (command == "mapList.load")
        return {"OK"};  // the list here is what mohw-bots.ini has
    if (command == "mapList.runNextRound" || command == "mapList.restartRound")
    {
        // The game restarts with that map (relaunch.h); it says so in chat first.
        if (command == "mapList.runNextRound" ? maps.empty() : g_snapshot.map.empty())
            return {command == "mapList.runNextRound" ? "InvalidMapIndex" : "LevelNotLoaded"};
        g_commands.push_back(w);
        return {"OK"};
    }
    return {"NotSupportedOnThisServer"};  // endRound
}

Words Help()
{
    Words w = {"OK"};
    for (const char* c : {"login.plainText", "login.hashed", "logout", "quit", "version", "serverInfo", "listPlayers",
                          "currentLevel", "admin.help", "admin.eventsEnabled", "admin.listPlayers", "admin.killPlayer",
                          "admin.kickPlayer", "admin.say", "admin.yell", "mapList.list", "mapList.getMapIndices",
                          "mapList.getRounds", "mapList.setNextMapIndex", "mapList.add", "mapList.remove", "mapList.clear",
                          "mapList.save", "mapList.load", "mapList.runNextRound", "mapList.restartRound",
                          "admin.getPlaylist", "admin.setPlaylist", "banList.list",
                          "reservedSlotsList.list", "nav.reset"})
        w.push_back(c);
    for (const auto& var : g_vars)
        w.push_back(var.first);
    return w;
}

Words Handle(Connection& c, const Words& w)
{
    if (w.empty())
        return {"InvalidArguments"};
    const std::string& command = w[0];
    Locked lock;

    // Open to anyone connected, as on a real server.
    if (command == "version")
        return {"OK", "MOHW", g_config.build};
    if (command == "serverInfo")
        return ServerInfo();
    if (command == "listPlayers")
        return ListPlayers(w);
    if (command == "login.plainText")
    {
        if (w.size() != 2)
            return {"InvalidArguments"};
        if (!g_config.password.empty() && w[1] != g_config.password)
            return {"InvalidPassword"};
        c.loggedIn = true;
        return {"OK"};
    }
    if (command == "login.hashed")
    {
        if (w.size() == 1)
        {
            NewSalt(c.salt, sizeof(c.salt));
            return {"OK", Hex(c.salt, sizeof(c.salt))};
        }
        if (w.size() != 2)
            return {"InvalidArguments"};
        if (!g_config.password.empty() && w[1] != HashedPassword(c.salt, sizeof(c.salt), g_config.password))
            return {"InvalidPasswordHash"};
        c.loggedIn = true;
        return {"OK"};
    }
    if (command == "logout")
    {
        c.loggedIn = false;
        return {"OK"};
    }
    if (command == "quit")
    {
        c.closing = true;
        return {"OK"};
    }
    if (!c.loggedIn)
        return {"LogInRequired"};

    if (command == "admin.eventsEnabled")
    {
        if (w.size() == 1)
            return {"OK", Bool(c.events)};
        c.events = w[1] == "true";
        return {"OK"};
    }
    if (command == "admin.help")
        return Help();
    if (command == "admin.listPlayers")
        return ListPlayers(w);
    if (command == "currentLevel")
        return {"OK", g_snapshot.map};
    if (command == "admin.killPlayer" || command == "admin.kickPlayer")
    {
        if (w.size() < 2)
            return {"InvalidArguments"};
        const Player* p = FindPlayer(w[1]);
        if (!p)
            return {"InvalidPlayerName"};
        if (command == "admin.kickPlayer" && !p->bot)
            return {"CannotKickAHuman"};
        g_commands.push_back(w);
        return {"OK"};
    }
    if (command == "admin.say" || command == "admin.yell")
    {
        if (w.size() < 2)
            return {"InvalidArguments"};
        g_commands.push_back(w);
        return {"OK"};
    }
    if (command == "nav.reset")
    {
        g_commands.push_back(w);  // the game says how it went in chat
        return {"OK"};
    }
    if (command.rfind("mapList.", 0) == 0)
        return MapListCommand(w);
    if (command == "admin.getPlaylist" || (command == "vars.playlist" && w.size() == 1))
        return {"OK", g_playlist};
    if (command == "admin.setPlaylist" || command == "vars.playlist")
    {
        if (w.size() != 2 || w[1].empty())
            return {"InvalidArguments"};
        std::string map = "MP_10", mode = w[1];  // Sarajevo Stadium has every mode: this only checks the name
        if (g_config.checkMap && g_config.checkMap(map, mode))
            return {"InvalidPlaylist"};
        g_playlist = mode;
        return {"OK"};
    }
    if (command == "banList.list" || command == "reservedSlotsList.list" || command.rfind("punkBuster.", 0) == 0)
        return {"OK"};

    auto var = g_vars.find(command);
    if (var != g_vars.end())
    {
        if (w.size() == 1)
            return {"OK", var->second.value};
        if (!var->second.writable)
            return {"CommandIsReadOnly"};
        var->second.value = w[1];
        g_commands.push_back(w);
        return {"OK"};
    }
    if (command.rfind("mapList.", 0) == 0 || command.rfind("admin.", 0) == 0 || command.rfind("banList.", 0) == 0 ||
        command.rfind("reservedSlotsList.", 0) == 0 || command.rfind("squad.", 0) == 0 || command.rfind("player.", 0) == 0)
        return {"NotSupportedOnThisServer"};
    return {"UnknownCommand"};
}

void ReadPackets(Connection& c)
{
    while (c.in.size() >= 12 && !c.closing)
    {
        uint32_t header = 0, size = 0;
        std::memcpy(&header, c.in.data(), 4);
        std::memcpy(&size, c.in.data() + 4, 4);
        if (size < 12 || size > kMaxPacket)
        {
            Say("rcon: a client sent a packet of %s%d bytes; closing it", "", static_cast<int>(size));
            c.closing = true;
            return;
        }
        if (c.in.size() < size)
            return;
        Words words;
        bool good = Decode(c.in.data(), size, &words);
        c.in.erase(0, size);
        if (!good)
        {
            Say("rcon: a client sent a malformed packet; closing it");
            c.closing = true;
            return;
        }
        if (header & kResponse)
            continue;  // its answer to one of our events
        Send(c, Encode((header & kSequenceMask) | kResponse | (header & kFromServer), Handle(c, words)));
    }
}

DWORD WINAPI ServerThread(void*)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        Say("rcon: WSAStartup failed");
        return 0;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(g_config.port));
    if (listener == INVALID_SOCKET || inet_pton(AF_INET, g_config.bind.c_str(), &address.sin_addr) != 1 ||
        bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 4) == SOCKET_ERROR)
    {
        char text[160];
        std::snprintf(text, sizeof(text), "rcon: can't listen on %s:%d (error %d); ProCon won't connect",
                      g_config.bind.c_str(), g_config.port, WSAGetLastError());
        if (g_config.log)
            g_config.log(text);
        return 0;
    }
    u_long nonBlocking = 1;
    ioctlsocket(listener, FIONBIO, &nonBlocking);
    char started[160];
    std::snprintf(started, sizeof(started), "rcon: listening on %s:%d, answering as MOHW build %s (%s)",
                  g_config.bind.c_str(), g_config.port, g_config.build.c_str(),
                  g_config.password.empty() ? "any password accepted" : "password set");
    if (g_config.log)
        g_config.log(started);

    std::vector<Connection> connections;
    for (;;)
    {
        fd_set in;
        FD_ZERO(&in);
        FD_SET(listener, &in);
        for (const Connection& c : connections)
            FD_SET(c.socket, &in);
        timeval wait = {0, 50000};  // events go out within 50 ms
        if (select(0, &in, nullptr, nullptr, &wait) == SOCKET_ERROR)
        {
            Sleep(50);
            continue;
        }
        if (FD_ISSET(listener, &in) && connections.size() < 8)
        {
            sockaddr_in from = {};
            int fromSize = sizeof(from);
            SOCKET accepted = accept(listener, reinterpret_cast<sockaddr*>(&from), &fromSize);
            if (accepted != INVALID_SOCKET)
            {
                ioctlsocket(accepted, FIONBIO, &nonBlocking);
                int noDelay = 1;
                setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
                Connection c;
                c.socket = accepted;
                c.loggedIn = false;
                connections.push_back(c);
                char ip[48] = "?";
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                Say("rcon: %s connected (%d connections)", ip, static_cast<int>(connections.size()));
            }
        }
        for (Connection& c : connections)
        {
            if (!FD_ISSET(c.socket, &in))
                continue;
            char buffer[8192];
            int n = recv(c.socket, buffer, sizeof(buffer), 0);
            if (n <= 0)
            {
                if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
                    c.closing = true;
                continue;
            }
            c.in.append(buffer, static_cast<size_t>(n));
            ReadPackets(c);
        }

        std::vector<Words> events;
        {
            Locked lock;
            events.swap(g_events);
        }
        for (const Words& event : events)
        {
            std::string packet = Encode(kFromServer | (g_eventSequence++ & kSequenceMask), event);
            for (Connection& c : connections)
                if (c.events && !c.closing)
                    Send(c, packet);
        }

        for (size_t i = connections.size(); i-- > 0;)
            if (connections[i].closing)
            {
                closesocket(connections[i].socket);
                connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(i));
                Say("rcon: a client left (%s%d connections)", "", static_cast<int>(connections.size()));
            }
    }
}

}  // namespace

bool Start(const Config& config)
{
    g_config = config;
    HANDLE thread = CreateThread(nullptr, 0, &ServerThread, nullptr, 0, nullptr);
    if (!thread)
    {
        Say("rcon: couldn't start its thread");
        return false;
    }
    CloseHandle(thread);
    return true;
}

void Publish(const Snapshot& snapshot)
{
    Locked lock;
    std::vector<MapEntry> maps;
    maps.swap(g_snapshot.maps);
    const int next = g_snapshot.nextMapIndex;
    g_snapshot = snapshot;
    g_snapshot.maps.swap(maps);  // the rotation is ours (SetMapList, then ProCon's edits)
    g_snapshot.nextMapIndex = next;
    g_snapshot.mapIndex = CurrentIndex();
}

void SetNextMapIndex(int nextMapIndex)
{
    Locked lock;
    if (!g_snapshot.maps.empty())
        g_snapshot.nextMapIndex = nextMapIndex % static_cast<int>(g_snapshot.maps.size());
}

void SetMapList(const std::vector<MapEntry>& maps, int nextMapIndex)
{
    Locked lock;
    if (g_playlist.empty())
        g_playlist = g_snapshot.mode.empty() ? "TeamDeathMatch" : g_snapshot.mode;
    g_snapshot.maps = maps;
    g_snapshot.nextMapIndex = maps.empty() ? 0 : nextMapIndex % static_cast<int>(maps.size());
    g_snapshot.mapIndex = CurrentIndex();
}

void SetVar(const std::string& name, const std::string& value, bool writable)
{
    Locked lock;
    Var& var = g_vars[name];
    var.value = value;
    var.writable = writable;
}

void Event(const Words& words)
{
    Locked lock;
    if (g_events.size() < 512)
        g_events.push_back(words);
}

std::vector<Words> TakeCommands()
{
    std::vector<Words> commands;
    Locked lock;
    commands.swap(g_commands);
    return commands;
}

}  // namespace rconsrv
