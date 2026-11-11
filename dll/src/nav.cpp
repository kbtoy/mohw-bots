#include "nav.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <queue>
#include <unordered_map>

#include "log.h"

namespace nav {
namespace {

constexpr uint8_t kEdgeJump = 1;
constexpr float kTrapPenalty = 64.0f;  // the highest penalty: a trap's links

struct Edge
{
    int to = -1;
    float length = 0.0f;
    float penalty = 1.0f;  // raised when bots get stuck on it
    uint8_t flags = 0;
};

struct Node
{
    float pos[3] = {};
    std::vector<Edge> edges;
    bool wet = false;  // water over it (ScanForWater): never routed through or linked to
    // A trap (MarkTrap, or on load every link into it at the highest penalty): never a route's goal or a spot to roam
    // or swim to, and a link learned into it later costs as much as the old ones.
    bool trap = false;
};

struct Walker
{
    void* player = nullptr;
    int lastNode = -1;
    float lastPos[3] = {};
    bool hasLast = false;
    bool jumped = false;  // airborne going up since the last node: the next link is a jump
};

struct Settings
{
    bool enabled = true;
    bool learn = true;
    bool learnFromBots = true;
    float spacing = 2.5f;   // metres between learned nodes
    float linkDistance = 5.0f;
    float saveSeconds = 30.0f;
    size_t maxNodes = 8000;
};

Settings g_settings;
std::vector<Node> g_nodes;
std::unordered_map<int64_t, std::vector<int>> g_grid;  // 4 m cells (x, z) -> node indices
constexpr float kCell = 4.0f;
std::vector<Walker> g_walkers;
std::string g_key;
bool g_dirty = false;
float g_saveTimer = 0.0f;
size_t g_learnedSinceLog = 0;

int ReadInt(const wchar_t* key, int fallback)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%smohw-bots.ini", ModuleDir());
    return static_cast<int>(GetPrivateProfileIntW(L"nav", key, fallback, path));
}

float Dist(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Dist2D(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

int Cell(float v) { return static_cast<int>(std::floor(v / kCell)); }
int64_t CellKey(int cx, int cz) { return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cz); }

// Nearest node within `radius` horizontally and `maxDy` vertically, or -1. With dyWeight > 0 they're ranked by
// horizontal distance + dyWeight x height difference, so a node on the same floor beats one on the floor overhead.
// skipTraps: not a trap (somewhere to go, not where a bot stands).
int Nearest(const float p[3], float radius, float maxDy, float dyWeight = 0.0f, bool skipTraps = false)
{
    int best = -1;
    float bestScore = 1e30f;
    int r = static_cast<int>(std::ceil(radius / kCell));
    int cx = Cell(p[0]), cz = Cell(p[2]);
    for (int x = cx - r; x <= cx + r; ++x)
        for (int z = cz - r; z <= cz + r; ++z)
        {
            auto it = g_grid.find(CellKey(x, z));
            if (it == g_grid.end())
                continue;
            for (int i : it->second)
            {
                const Node& n = g_nodes[i];
                float dy = std::fabs(n.pos[1] - p[1]);
                if (n.wet || dy > maxDy || (skipTraps && n.trap))
                    continue;
                float d = Dist2D(n.pos, p);
                float score = d + dyWeight * dy;
                if (d < radius && score < bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
    return best;
}

int AddNode(const float p[3])
{
    Node n;
    std::memcpy(n.pos, p, sizeof(n.pos));
    g_nodes.push_back(n);
    int index = static_cast<int>(g_nodes.size()) - 1;
    g_grid[CellKey(Cell(p[0]), Cell(p[2]))].push_back(index);
    return index;
}

Edge* FindEdge(int a, int b)
{
    for (Edge& e : g_nodes[a].edges)
        if (e.to == b)
            return &e;
    return nullptr;
}

void AddOneWay(int a, int b, uint8_t flags)
{
    if (a == b || FindEdge(a, b))
        return;
    Edge e;
    e.to = b;
    e.length = Dist(g_nodes[a].pos, g_nodes[b].pos);
    e.flags = flags;
    if (g_nodes[b].trap)
        e.penalty = kTrapPenalty;  // walking in again doesn't make a trap any easier to walk out of
    g_nodes[a].edges.push_back(e);
    g_dirty = true;
}

// The walker went a -> b. That direction is walkable as walked (a climb of more than 0.6 m is marked as a jump);
// the reverse only when it isn't a big drop (a ledge you dropped from can't be walked back up).
void Link(int a, int b, bool jumped)
{
    float rise = g_nodes[b].pos[1] - g_nodes[a].pos[1];
    AddOneWay(a, b, (jumped || rise > 0.6f) ? kEdgeJump : 0);
    if (std::fabs(rise) <= 1.2f)
        AddOneWay(b, a, -rise > 0.6f ? kEdgeJump : 0);
}

Walker& WalkerFor(void* player)
{
    for (Walker& w : g_walkers)
        if (w.player == player)
            return w;
    Walker w;
    w.player = player;
    g_walkers.push_back(w);
    return g_walkers.back();
}

std::wstring FilePath(const std::string& key)
{
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%smohw-bots-nav", ModuleDir());
    CreateDirectoryW(dir, nullptr);
    std::wstring path = dir;
    path += L"\\";
    for (char c : key)
        path += static_cast<wchar_t>(c);
    path += L".nav";
    return path;
}

size_t EdgeCount()
{
    size_t count = 0;
    for (const Node& n : g_nodes)
        count += n.edges.size();
    return count;
}

void Save()
{
    if (g_key.empty() || !g_dirty)
        return;
    std::wstring path = FilePath(g_key);
    std::wstring temp = path + L".tmp";
    FILE* f = _wfopen(temp.c_str(), L"w");
    if (!f)
    {
        Log("nav: can't write %ls", temp.c_str());
        return;
    }
    std::fprintf(f, "mohw-bots-nav 1 %s\nnodes %u\n", g_key.c_str(), static_cast<unsigned>(g_nodes.size()));
    for (const Node& n : g_nodes)
        std::fprintf(f, "%.2f %.2f %.2f\n", n.pos[0], n.pos[1], n.pos[2]);
    std::fprintf(f, "edges %u\n", static_cast<unsigned>(EdgeCount()));
    for (size_t i = 0; i < g_nodes.size(); ++i)
        for (const Edge& e : g_nodes[i].edges)
            std::fprintf(f, "%u %d %u %.2f\n", static_cast<unsigned>(i), e.to, e.flags, e.penalty);
    std::fclose(f);
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_dirty = false;
    Log("nav: saved %s: %u nodes, %u links (+%u nodes since last save)", g_key.c_str(),
        static_cast<unsigned>(g_nodes.size()), static_cast<unsigned>(EdgeCount()),
        static_cast<unsigned>(g_learnedSinceLog));
    g_learnedSinceLog = 0;
}

// After loading: a node is a trap when it has links into it and every one costs the most (traps aren't stored as
// such; their links are).
int FindTraps()
{
    std::vector<int> in(g_nodes.size(), 0), highest(g_nodes.size(), 0);
    for (const Node& n : g_nodes)
        for (const Edge& e : n.edges)
        {
            ++in[e.to];
            highest[e.to] += e.penalty >= kTrapPenalty;
        }
    int traps = 0;
    for (size_t i = 0; i < g_nodes.size(); ++i)
    {
        g_nodes[i].trap = in[i] > 0 && highest[i] == in[i];
        traps += g_nodes[i].trap;
    }
    return traps;
}

void Clear()
{
    g_nodes.clear();
    g_grid.clear();
    g_walkers.clear();
    g_dirty = false;
}

// "MP_03_SectorControl" -> "MP_03_TeamDeathMatch": the same map's Team Deathmatch graph (empty if key is that already).
// Game modes share the map's ground; their layers only add or remove some props and blockers.
std::string TeamDeathmatchKey(const std::string& key)
{
    if (key.size() < 7 || key.compare(0, 3, "MP_") != 0 || key[5] != '_')
        return {};
    std::string tdm = key.substr(0, 6) + "TeamDeathMatch";
    return tdm == key ? std::string() : tdm;
}

void Load(const std::string& key)
{
    Clear();
    std::wstring path = FilePath(key);
    FILE* f = _wfopen(path.c_str(), L"r");
    bool seeded = false;
    if (!f)
    {
        std::string tdm = TeamDeathmatchKey(key);
        if (!tdm.empty() && (f = _wfopen(FilePath(tdm).c_str(), L"r")) != nullptr)
        {
            seeded = true;
            Log("nav: %s: no saved graph yet; starting from %s's routes (saved as %s once it grows)", key.c_str(), tdm.c_str(),
                key.c_str());
        }
        else
        {
            Log("nav: %s: no saved graph yet; learning from players from scratch", key.c_str());
            return;
        }
    }
    char header[64] = {};
    unsigned version = 0, nodeCount = 0, edgeCount = 0;
    bool ok = std::fscanf(f, "%63s %u %*s nodes %u", header, &version, &nodeCount) == 3 &&
              std::strcmp(header, "mohw-bots-nav") == 0 && version == 1 && nodeCount <= 100000;
    for (unsigned i = 0; ok && i < nodeCount; ++i)
    {
        float p[3];
        ok = std::fscanf(f, "%f %f %f", &p[0], &p[1], &p[2]) == 3;
        if (ok)
            AddNode(p);
    }
    ok = ok && std::fscanf(f, " edges %u", &edgeCount) == 1;
    for (unsigned i = 0; ok && i < edgeCount; ++i)
    {
        unsigned a = 0, flags = 0;
        int b = 0;
        float penalty = 1.0f;
        ok = std::fscanf(f, "%u %d %u %f", &a, &b, &flags, &penalty) == 4 && a < g_nodes.size() && b >= 0 &&
             static_cast<size_t>(b) < g_nodes.size();
        if (ok)
        {
            AddOneWay(static_cast<int>(a), b, static_cast<uint8_t>(flags));
            if (Edge* e = FindEdge(static_cast<int>(a), b))
                e->penalty = penalty;
        }
    }
    std::fclose(f);
    g_dirty = seeded;  // a seeded graph is saved under its own name at the first save
    if (!ok)
    {
        Log("nav: %s: file %ls is damaged; starting empty", key.c_str(), seeded ? L"(the Team Deathmatch one)" : path.c_str());
        Clear();
        return;
    }
    int traps = FindTraps();
    Log("nav: %s: loaded %u nodes, %u links, %d trap spots", key.c_str(), static_cast<unsigned>(g_nodes.size()),
        static_cast<unsigned>(EdgeCount()), traps);
}

}  // namespace

void Init()
{
    g_settings.enabled = ReadInt(L"Enabled", 1) != 0;
    g_settings.learn = ReadInt(L"LearnFromPlayers", 1) != 0;
    g_settings.learnFromBots = ReadInt(L"LearnFromBots", 1) != 0;
    g_settings.spacing = ReadInt(L"NodeSpacingCm", 250) / 100.0f;
    g_settings.saveSeconds = static_cast<float>(ReadInt(L"SaveSeconds", 30));
    g_settings.maxNodes = static_cast<size_t>(ReadInt(L"MaxNodes", 8000));
    g_settings.linkDistance = g_settings.spacing * 2.0f;
    Log("nav: %s, learning from players %s, from bots %s, node spacing %.1f m, save every %.0f s, max %u nodes",
        g_settings.enabled ? "on" : "off", g_settings.learn ? "on" : "off", g_settings.learnFromBots ? "on" : "off",
        g_settings.spacing, g_settings.saveSeconds, static_cast<unsigned>(g_settings.maxNodes));
}

void OnLevel(const std::string& key)
{
    if (key == g_key)
        return;
    Save();
    g_key = key;
    if (g_settings.enabled && !key.empty())
        Load(key);
    else
        Clear();
}

int ResetMap(const std::string& map, bool* wasCurrent)
{
    const std::string prefix = map + "_";
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf(stamp, 32, L".reset-%04u%02u%02u-%02u%02u%02u.bak", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%smohw-bots-nav", ModuleDir());
    std::wstring pattern = dir;
    pattern += L"\\";
    for (char c : prefix)
        pattern += static_cast<wchar_t>(c);
    pattern += L"*";
    int files = 0;
    WIN32_FIND_DATAW found;
    std::vector<std::wstring> names;
    HANDLE find = FindFirstFileW(pattern.c_str(), &found);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::wstring name = found.cFileName;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, L".nav") == 0)  // not the .bak copies
                names.push_back(name);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    for (const std::wstring& name : names)
    {
        std::wstring from = std::wstring(dir) + L"\\" + name;
        if (MoveFileW(from.c_str(), (from + stamp).c_str()))
            ++files;
    }
    const bool current = g_key.compare(0, prefix.size(), prefix) == 0;
    if (current)
        Clear();  // a bot's route in hand holds positions, and node numbers only Penalize uses, which checks them
    if (wasCurrent)
        *wasCurrent = current;
    return files;
}

void Learn(void* player, const float feet[3], float dt)
{
    if (!g_settings.enabled || !g_settings.learn || g_key.empty())
        return;
    Walker& w = WalkerFor(player);
    if (!w.hasLast)
    {
        std::memcpy(w.lastPos, feet, sizeof(w.lastPos));
        w.hasLast = true;
        w.lastNode = -1;
        return;
    }
    float step = Dist(feet, w.lastPos);
    float vy = (feet[1] - w.lastPos[1]) / (dt > 0.001f ? dt : 0.001f);
    std::memcpy(w.lastPos, feet, sizeof(w.lastPos));
    if (step > 5.0f)
    {
        w.lastNode = -1;  // respawned or teleported: start a new chain
        return;
    }
    if (vy > 2.0f)
        w.jumped = true;
    if (std::fabs(vy) > 2.5f)
        return;  // in the air: no nodes mid-jump or mid-fall

    int closest = Nearest(feet, g_settings.spacing * 0.7f, 1.2f);
    if (closest >= 0)
    {
        if (w.lastNode >= 0 && closest != w.lastNode &&
            Dist(g_nodes[w.lastNode].pos, g_nodes[closest].pos) < g_settings.linkDistance)
            Link(w.lastNode, closest, w.jumped);
        if (closest != w.lastNode)
            w.jumped = false;
        w.lastNode = closest;
        return;
    }
    if (w.lastNode >= 0 && Dist2D(feet, g_nodes[w.lastNode].pos) < g_settings.spacing &&
        std::fabs(feet[1] - g_nodes[w.lastNode].pos[1]) < 1.2f)
        return;
    if (g_nodes.size() >= g_settings.maxNodes)
        return;
    int n = AddNode(feet);
    if (w.lastNode >= 0 && Dist(g_nodes[w.lastNode].pos, feet) < g_settings.linkDistance)
        Link(w.lastNode, n, w.jumped);
    w.jumped = false;
    w.lastNode = n;
    g_dirty = true;
    ++g_learnedSinceLog;
}

void ForgetWalker(void* player)
{
    for (size_t i = 0; i < g_walkers.size(); ++i)
        if (g_walkers[i].player == player)
        {
            g_walkers.erase(g_walkers.begin() + i);
            return;
        }
}

void Tick(float dt)
{
    g_saveTimer += dt;
    if (g_saveTimer < g_settings.saveSeconds)
        return;
    g_saveTimer = 0.0f;
    Save();
}

size_t NodeCount() { return g_settings.enabled ? g_nodes.size() : 0; }

bool LearnFromBots() { return g_settings.enabled && g_settings.learnFromBots; }

bool FindPath(const float from[3], const float to[3], std::vector<Waypoint>& out, int* failure)
{
    out.clear();
    if (!g_settings.enabled || g_nodes.empty())
        return false;
    // Height counts double: a bot hopping under a ledge used to start its route on the ledge right above its head.
    int start = Nearest(from, 6.0f, 2.0f, 2.0f);
    if (start < 0)
        start = Nearest(from, 12.0f, 3.0f, 2.0f);
    // Never a trap as the goal: a bot sent to one (a roam point, an enemy standing in it) had to walk in.
    int goal = Nearest(to, 12.0f, 3.0f, 2.0f, true);
    if (start < 0 || goal < 0)
    {
        if (failure)
            *failure = start < 0 ? 1 : 2;
        return false;
    }

    size_t n = g_nodes.size();
    std::vector<float> cost(n, 1e30f);
    std::vector<int> from_(n, -1);
    std::vector<uint8_t> closed(n, 0);
    using Item = std::pair<float, int>;  // (estimated total, node)
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    cost[start] = 0.0f;
    open.push({Dist(g_nodes[start].pos, g_nodes[goal].pos), start});
    while (!open.empty())
    {
        int current = open.top().second;
        open.pop();
        if (closed[current])
            continue;
        closed[current] = 1;
        if (current == goal)
            break;
        for (const Edge& e : g_nodes[current].edges)
        {
            float c = cost[current] + e.length * e.penalty;
            if (closed[e.to] || c >= cost[e.to] || g_nodes[e.to].wet)
                continue;
            cost[e.to] = c;
            from_[e.to] = current;
            open.push({c + Dist(g_nodes[e.to].pos, g_nodes[goal].pos), e.to});
        }
    }
    if (!closed[goal])
    {
        if (failure)
            *failure = 3;
        return false;
    }

    std::vector<int> chain;
    for (int i = goal; i >= 0; i = from_[i])
        chain.push_back(i);
    for (size_t k = chain.size(); k-- > 0;)
    {
        Waypoint w;
        w.node = chain[k];
        std::memcpy(w.pos, g_nodes[w.node].pos, sizeof(w.pos));
        if (k + 1 < chain.size())
            if (const Edge* e = FindEdge(chain[k + 1], w.node))
                w.jump = (e->flags & kEdgeJump) != 0;
        out.push_back(w);
    }
    return true;
}

bool NearestNode(const float p[3], float radius, float out[3])
{
    int i = g_settings.enabled ? Nearest(p, radius, 10.0f, 0.0f, true) : -1;
    if (i < 0)
        return false;
    std::memcpy(out, g_nodes[i].pos, sizeof(g_nodes[i].pos));
    return true;
}

bool NearestNodeAvoiding(const float p[3], float radius, const float (*avoid)[3], int avoidCount, float minSep,
                         float out[3], float maxRise)
{
    if (!g_settings.enabled)
        return false;
    int best = -1;
    float bestDist = radius;
    int r = static_cast<int>(std::ceil(radius / kCell));
    int cx = Cell(p[0]), cz = Cell(p[2]);
    for (int x = cx - r; x <= cx + r; ++x)
        for (int z = cz - r; z <= cz + r; ++z)
        {
            auto it = g_grid.find(CellKey(x, z));
            if (it == g_grid.end())
                continue;
            for (int i : it->second)
            {
                const Node& n = g_nodes[i];
                if (n.wet || n.trap || std::fabs(n.pos[1] - p[1]) > 10.0f || n.pos[1] - p[1] > maxRise)
                    continue;
                float d = Dist2D(n.pos, p);
                if (d >= bestDist)
                    continue;
                bool nearFailed = false;
                for (int k = 0; k < avoidCount && !nearFailed; ++k)
                    nearFailed = Dist2D(n.pos, avoid[k]) < minSep;
                if (nearFailed)
                    continue;
                bestDist = d;
                best = i;
            }
        }
    if (best < 0)
        return false;
    std::memcpy(out, g_nodes[best].pos, sizeof(g_nodes[best].pos));
    return true;
}

bool RandomNode(uint32_t seed, const float p[3], float minDist, float maxDist, float out[3])
{
    if (!g_settings.enabled || g_nodes.empty())
        return false;
    uint32_t x = seed ? seed : 0x9E3779B9u;
    for (int tries = 0; tries < 64; ++tries)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        const Node& n = g_nodes[x % g_nodes.size()];
        float d = Dist2D(n.pos, p);
        if (n.wet || n.trap || d < minDist || d > maxDist || n.edges.empty())
            continue;
        std::memcpy(out, n.pos, sizeof(n.pos));
        return true;
    }
    return false;
}

int ScanForWater(float (*depthAt)(const float pos[3]), int count, float maxDepth)
{
    static size_t next = 0;
    int marked = 0;
    for (int k = 0; k < count && !g_nodes.empty(); ++k)
    {
        if (next >= g_nodes.size())
            next = 0;
        Node& n = g_nodes[next++];
        bool wet = depthAt(n.pos) > maxDepth;
        marked += wet && !n.wet;
        n.wet = wet;
    }
    return marked;
}

void Penalize(int a, int b)
{
    if (a < 0 || b < 0 || static_cast<size_t>(a) >= g_nodes.size() || static_cast<size_t>(b) >= g_nodes.size())
        return;
    if (Edge* e = FindEdge(a, b))
    {
        e->penalty = e->penalty * 2.0f > kTrapPenalty ? kTrapPenalty : e->penalty * 2.0f;
        g_dirty = true;
    }
}

int MarkTrap(const float pos[3])
{
    if (!g_settings.enabled)
        return 0;
    std::vector<uint8_t> trap(g_nodes.size(), 0);
    for (size_t i = 0; i < g_nodes.size(); ++i)
    {
        trap[i] = Dist2D(g_nodes[i].pos, pos) < 1.5f && std::fabs(g_nodes[i].pos[1] - pos[1]) < 1.5f;
        if (trap[i])
            g_nodes[i].trap = true;
    }
    int raised = 0;
    for (Node& n : g_nodes)
        for (Edge& e : n.edges)
            if (trap[e.to] && e.penalty < kTrapPenalty)
            {
                e.penalty = kTrapPenalty;
                ++raised;
            }
    if (raised)
        g_dirty = true;
    return raised;
}

}  // namespace nav
