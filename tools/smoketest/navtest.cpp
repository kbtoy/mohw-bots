// Offline test of the bot navigation graph (dll/src/nav.cpp): a simulated player walks routes, then the graph,
// A* routes, jump links, penalties and the save/load round trip are checked. Builds nav.cpp directly with stub
// logging; writes to a temp folder.

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "../../dll/src/nav.h"

namespace {

wchar_t g_dir[MAX_PATH];
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// Walks `player` from a to b at 4 m/s in 1/60 s ticks, optionally rising by `rise` with a jump half way.
void Walk(void* player, const float a[3], const float b[3], float rise = 0.0f)
{
    float dx = b[0] - a[0], dz = b[2] - a[2];
    float length = std::sqrt(dx * dx + dz * dz);
    int ticks = static_cast<int>(length / 4.0f * 60.0f);
    for (int i = 0; i <= ticks; ++i)
    {
        float t = ticks ? static_cast<float>(i) / ticks : 1.0f;
        float p[3] = {a[0] + dx * t, a[1] + (b[1] - a[1]) * t, a[2] + dz * t};
        if (rise > 0.0f && t > 0.45f && t < 0.55f)
            p[1] += std::sin((t - 0.45f) / 0.1f * 3.14159f) * 1.0f;  // a hop
        nav::Learn(player, p, 1.0f / 60.0f);
    }
}

}  // namespace

// Stubs for the DLL's logging.
void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::printf("  log: ");
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
}

const wchar_t* ModuleDir() { return g_dir; }

int main()
{
    GetTempPathW(MAX_PATH, g_dir);
    wcscat_s(g_dir, L"mohw-navtest\\");
    CreateDirectoryW(g_dir, nullptr);
    std::wstring file = std::wstring(g_dir) + L"mohw-bots-nav\\TestMap_TDM.nav";
    DeleteFileW(file.c_str());

    nav::Init();
    nav::OnLevel("TestMap_TDM");
    Check(nav::NodeCount() == 0, "new map starts empty");

    // An L-shaped corridor: (0,0,0) -> (40,0,0) -> (40,0,30), plus a ledge climb (40,0,30) -> (40,1,40).
    int player = 0;
    float a[3] = {0, 0, 0}, b[3] = {40, 0, 0}, c[3] = {40, 0, 30}, d[3] = {40, 1, 40};
    Walk(&player, a, b);
    Walk(&player, b, c);
    Walk(&player, c, d, 1.0f);
    size_t nodes = nav::NodeCount();
    std::printf("  %u nodes after walking ~80 m\n", static_cast<unsigned>(nodes));
    Check(nodes >= 25 && nodes <= 40, "about one node per 2.5 m walked");

    // Walking the same corridor back must reuse nodes, not double them.
    Walk(&player, c, b);
    Check(nav::NodeCount() <= nodes + 2, "walking a known route back adds (almost) no nodes");

    std::vector<nav::Waypoint> path;
    float from[3] = {1, 0, 1}, to[3] = {39, 0, 29};
    Check(nav::FindPath(from, to, path), "route found along the corridor");
    bool turnsCorner = false;
    for (const nav::Waypoint& w : path)
        turnsCorner |= std::fabs(w.pos[0] - 40.0f) < 2.0f && std::fabs(w.pos[2]) < 2.0f;
    Check(path.size() >= 20 && turnsCorner, "route follows the L (goes via the corner, not through the wall)");

    // Reverse direction works too (links are two-way on flat ground).
    std::vector<nav::Waypoint> back;
    Check(nav::FindPath(to, from, back) && back.size() >= 20, "route back the other way");

    // The climb to the ledge was a jump: the waypoint after it is flagged.
    std::vector<nav::Waypoint> up;
    float ledge[3] = {40, 1, 39.5f};
    bool foundJump = false;
    if (nav::FindPath(from, ledge, up))
        for (const nav::Waypoint& w : up)
            foundJump |= w.jump;
    Check(foundJump, "the hop onto the ledge is marked as a jump link");

    // Unconnected areas: no route.
    int other = 0;
    float e[3] = {200, 0, 200}, f[3] = {220, 0, 200};
    Walk(&other, e, f);
    std::vector<nav::Waypoint> none;
    Check(!nav::FindPath(from, f, none), "no route between areas nobody walked between");

    // Penalising a link makes routes avoid it when there's an alternative: add a parallel corridor at z = 6.
    int third = 0;
    float g[3] = {0, 0, 6}, h[3] = {40, 0, 6};
    Walk(&third, a, g);
    Walk(&third, g, h);
    Walk(&third, h, b);
    std::vector<nav::Waypoint> first;
    nav::FindPath(from, b, first);
    bool viaSouth = false;
    for (const nav::Waypoint& w : first)
        viaSouth |= std::fabs(w.pos[2] - 6.0f) < 1.0f;
    for (int i = 0; i < 6; ++i)
        for (size_t k = 1; k < first.size(); ++k)
            nav::Penalize(first[k - 1].node, first[k].node);
    std::vector<nav::Waypoint> second;
    nav::FindPath(from, b, second);
    bool secondViaSouth = false;
    for (const nav::Waypoint& w : second)
        secondViaSouth |= std::fabs(w.pos[2] - 6.0f) < 1.0f;
    Check(viaSouth != secondViaSouth, "penalised links push the route onto the other corridor");

    // Save, switch level, come back: same graph, penalties kept.
    size_t before = nav::NodeCount();
    nav::OnLevel("OtherMap_TDM");
    Check(nav::NodeCount() == 0, "another map has its own (empty) graph");
    nav::OnLevel("TestMap_TDM");
    Check(nav::NodeCount() == before, "graph reloaded from disk with every node");
    std::vector<nav::Waypoint> third2;
    nav::FindPath(from, b, third2);
    bool reloadedViaSouth = false;
    for (const nav::Waypoint& w : third2)
        reloadedViaSouth |= std::fabs(w.pos[2] - 6.0f) < 1.0f;
    Check(reloadedViaSouth == secondViaSouth, "penalties survive the save/load round trip");

    // Respawning (a jump of more than 5 m) must not link the old spot to the new one.
    int fourth = 0;
    float p1[3] = {500, 0, 500}, p2[3] = {510, 0, 500}, p3[3] = {600, 0, 600}, p4[3] = {610, 0, 600};
    Walk(&fourth, p1, p2);
    Walk(&fourth, p3, p4);  // teleport between
    std::vector<nav::Waypoint> across;
    Check(!nav::FindPath(p1, p4, across), "a respawn doesn't create a link across the map");

    // Two floors, the upper one 1.9 m up and a metre over: a bot hopping on the lower floor (feet 0.6 m up), right
    // under an upper node, starts its route on its own floor - the floors aren't connected, so an upper start fails.
    int lower = 0, upper = 0;
    float l1[3] = {300, 0, 301}, l2[3] = {310, 0, 301}, u1[3] = {300, 1.9f, 300}, u2[3] = {310, 1.9f, 300};
    Walk(&lower, l1, l2);
    Walk(&upper, u1, u2);
    float hopping[3] = {300, 0.6f, 300.1f};
    std::vector<nav::Waypoint> ownFloor;
    Check(nav::FindPath(hopping, l2, ownFloor) && !ownFloor.empty() && ownFloor[0].pos[1] < 1.0f,
          "a route starts on the bot's own floor, not the one overhead");

    // A square loop: two equally long ways between opposite corners. Marking a trap at a corner the route goes via
    // (a bot stuck there 9 s) sends the route the other way.
    int looper = 0;
    float q1[3] = {600, 0, 600}, q2[3] = {620, 0, 600}, q3[3] = {620, 0, 620}, q4[3] = {600, 0, 620};
    Walk(&looper, q1, q2);
    Walk(&looper, q2, q3);
    Walk(&looper, q3, q4);
    Walk(&looper, q4, q1);
    std::vector<nav::Waypoint> around;
    nav::FindPath(q1, q3, around);
    bool viaQ2 = false;
    for (const nav::Waypoint& w : around)
        viaQ2 |= std::fabs(w.pos[0] - 620.0f) < 1.0f && std::fabs(w.pos[2] - 600.0f) < 1.0f;
    const float* corner = viaQ2 ? q2 : q4;
    int raised = nav::MarkTrap(corner);
    std::vector<nav::Waypoint> aroundAgain;
    bool avoided = nav::FindPath(q1, q3, aroundAgain);
    for (const nav::Waypoint& w : aroundAgain)
        avoided &= std::hypot(w.pos[0] - corner[0], w.pos[2] - corner[2]) >= 1.5f;
    Check(!around.empty() && raised > 0 && avoided, "a marked trap drops out of routes when there's another way");

    // A straight line a - t - b with a trap marked at t. A route sent to t ends next to it instead.
    int liner = 0;
    float ta[3] = {800, 0, 800}, tt[3] = {810, 0, 800}, tb[3] = {820, 0, 800};
    Walk(&liner, ta, tb);
    nav::MarkTrap(tt);
    std::vector<nav::Waypoint> intoTrap;
    Check(nav::FindPath(ta, tt, intoTrap) && !intoTrap.empty() &&
              std::hypot(intoTrap.back().pos[0] - tt[0], intoTrap.back().pos[2] - tt[2]) >= 1.5f,
          "a trap is never where a route ends (a bot sent there stops next to it)");

    // Reloaded, then bots walk a new way into t from the south (s -> t) and a longer one round it that joins the
    // line past t (s -> r -> j -> g). Via t, s -> g is 15 m against 25 m round, but the new link into t costs as much
    // as the old ones, so the route goes round.
    nav::OnLevel("OtherMap_TDM");
    nav::OnLevel("TestMap_TDM");
    int spur = 0, round = 0;
    float ts[3] = {810, 0, 790}, tr[3] = {822, 0, 790}, tj[3] = {818, 0, 800}, tg[3] = {814, 0, 800};
    Walk(&spur, ts, tt);
    Walk(&round, ts, tr);
    Walk(&round, tr, tj);
    Walk(&round, tj, tg);
    std::vector<nav::Waypoint> past;
    bool wentRound = nav::FindPath(ts, tg, past) && past.size() >= 6;
    for (const nav::Waypoint& w : past)
        wentRound &= std::hypot(w.pos[0] - tt[0], w.pos[2] - tt[2]) >= 1.5f;
    Check(wentRound, "a link learned into a trap later (even after a reload) costs as much as the old ones");

    // Resetting a map: its file is set aside, the graph empties, and it learns again.
    nav::OnLevel("OtherMap_TDM");  // saves TestMap_TDM
    nav::OnLevel("TestMap_TDM");
    size_t nodesBefore = nav::NodeCount();
    bool wasCurrent = false;
    int setAside = nav::ResetMap("TestMap", &wasCurrent);
    Check(nodesBefore > 0 && setAside == 1 && wasCurrent && nav::NodeCount() == 0, "reset: the map being played empties, its file set aside");
    Check(GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES, "reset: the .nav file is gone");
    WIN32_FIND_DATAW kept;
    std::wstring keptPattern = file + L".reset-*.bak";
    HANDLE keptFile = FindFirstFileW(keptPattern.c_str(), &kept);
    Check(keptFile != INVALID_HANDLE_VALUE, "reset: the old routes are kept as a .bak");
    if (keptFile != INVALID_HANDLE_VALUE)
    {
        do
            DeleteFileW((std::wstring(g_dir) + L"mohw-bots-nav\\" + kept.cFileName).c_str());
        while (FindNextFileW(keptFile, &kept));
        FindClose(keptFile);
    }
    int again = 0;
    float ra[3] = {900, 0, 900}, rb[3] = {920, 0, 900};
    Walk(&again, ra, rb);
    Check(nav::NodeCount() > 0, "reset: it learns again straight away");
    nav::OnLevel("OtherMap_TDM");
    nav::OnLevel("TestMap_TDM");
    wasCurrent = true;
    nav::OnLevel("OtherMap_TDM");
    int otherMap = nav::ResetMap("TestMap", &wasCurrent);
    Check(otherMap == 1 && !wasCurrent, "reset: another map's file can be set aside while this one plays on");
    keptFile = FindFirstFileW(keptPattern.c_str(), &kept);
    if (keptFile != INVALID_HANDLE_VALUE)
    {
        do
            DeleteFileW((std::wstring(g_dir) + L"mohw-bots-nav\\" + kept.cFileName).c_str());
        while (FindNextFileW(keptFile, &kept));
        FindClose(keptFile);
    }

    // A game mode played for the first time starts from the same map's Team Deathmatch routes, saved under its own name.
    nav::OnLevel("MP_77_TeamDeathMatch");
    int tdmWalker = 0;
    float sa[3] = {0, 0, 500}, sb[3] = {30, 0, 500};
    Walk(&tdmWalker, sa, sb);
    const size_t tdmNodes = nav::NodeCount();
    nav::OnLevel("OtherMap_TDM");  // saves MP_77_TeamDeathMatch
    nav::OnLevel("MP_77_SectorControl");
    Check(tdmNodes > 0 && nav::NodeCount() == tdmNodes, "a new game mode starts from the map's Team Deathmatch routes");
    nav::OnLevel("OtherMap_TDM");  // saves the seeded graph
    const std::wstring tdmFile = std::wstring(g_dir) + L"mohw-bots-nav\\MP_77_TeamDeathMatch.nav";
    const std::wstring scFile = std::wstring(g_dir) + L"mohw-bots-nav\\MP_77_SectorControl.nav";
    Check(GetFileAttributesW(scFile.c_str()) != INVALID_FILE_ATTRIBUTES, "...and saves them as its own file");
    DeleteFileW(tdmFile.c_str());
    DeleteFileW(scFile.c_str());
    nav::OnLevel("MP_77_CombatMission");
    Check(nav::NodeCount() == 0, "with no Team Deathmatch routes either, a mode starts empty");
    nav::OnLevel("OtherMap_TDM");

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
