#include "modes.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "log.h"

namespace modes {
namespace {

struct ModeInfo
{
    const char* name;
    const char* display;
};
const ModeInfo kModes[] = {
    {"TeamDeathMatch", "Team Deathmatch"}, {"CombatMission", "Combat Mission"}, {"SectorControl", "Sector Control"},
    {"BombSquad", "Hot Spot"},             {"Sport", "Home Run"},
};

// Which sub-levels each map's .toc lists (mp_xx_tdm, _cm, _sc, _bs / _bombsquad, _sm / _sport; tools/objectives.py
// docstring). ProCon's MOHW.def lists the same: no Home Run on Novi Grad, no Hot Spot on Basilan.
struct MapInfo
{
    const char* code;
    const char* name;
    bool homeRun;
    bool hotSpot;
};
const MapInfo kMaps[] = {
    {"MP_03", "Somalia Stronghold", true, true}, {"MP_05", "Novi Grad Warzone", false, true},
    {"MP_10", "Sarajevo Stadium", true, true},   {"MP_12", "Basilan Aftermath", true, false},
    {"MP_13", "Hara Dunes", true, true},         {"MP_16", "Al Fara Cliffside", true, true},
    {"MP_18", "Shogore Valley", true, true},     {"MP_19", "Tungawan Jungle", true, true},
    {"MP_20", "Darra Gun Market", true, true},   {"MP_21", "Chitral Compound", true, true},
};

std::string g_launchMap;
std::string g_launchMode = "TeamDeathMatch";

const MapInfo* FindMapInfo(const std::string& map)
{
    for (const MapInfo& m : kMaps)
        if (_stricmp(m.code, map.c_str()) == 0)
            return &m;
    return nullptr;
}

std::string Narrow(const wchar_t* wide)
{
    char text[512] = {};
    WideCharToMultiByte(CP_ACP, 0, wide, -1, text, sizeof(text), nullptr, nullptr);
    return text;
}

std::string DefaultMode(const wchar_t* ini)
{
    wchar_t wide[64] = {};
    GetPrivateProfileStringW(L"launch", L"Mode", L"TeamDeathMatch", wide, 64, ini);
    std::string mode = Canonical(Narrow(wide));
    return mode.empty() ? "TeamDeathMatch" : mode;
}

}  // namespace

const char* DisplayName(const std::string& mode)
{
    for (const ModeInfo& m : kModes)
        if (mode == m.name)
            return m.display;
    return mode.c_str();
}

const char* MapName(const std::string& map)
{
    const MapInfo* m = FindMapInfo(map);
    return m ? m->name : nullptr;
}

std::string Canonical(const std::string& mode)
{
    for (const ModeInfo& m : kModes)
        if (_stricmp(m.name, mode.c_str()) == 0)
            return m.name;
    return {};
}

std::string CanonicalMap(const std::string& map)
{
    const MapInfo* m = FindMapInfo(map);
    return m ? m->code : std::string();
}

bool MapHasMode(const std::string& map, const std::string& mode)
{
    const MapInfo* m = FindMapInfo(map);
    if (!m)
        return false;
    if (mode == "Sport")
        return m->homeRun;
    if (mode == "BombSquad")
        return m->hotSpot;
    return mode == "TeamDeathMatch" || mode == "CombatMission" || mode == "SectorControl";
}

bool ParseEntry(const std::string& text, const std::string& fallbackMode, const char* where, Entry* out)
{
    const size_t colon = text.find(':');
    std::string map = CanonicalMap(text.substr(0, colon));
    std::string asked = colon == std::string::npos ? fallbackMode : text.substr(colon + 1);
    std::string mode = Canonical(asked);
    if (map.empty())
    {
        Log("modes: %s entry \"%s\": not a map these bots know; skipped", where, text.c_str());
        return false;
    }
    if (mode.empty() || !MapHasMode(map, mode))
    {
        Log("modes: %s entry \"%s\": %s has no \"%s\" mode; it plays Team Deathmatch", where, text.c_str(), map.c_str(),
            asked.c_str());
        mode = "TeamDeathMatch";
    }
    out->map = map;
    out->mode = mode;
    return true;
}

std::vector<Entry> ReadRotation(const wchar_t* ini)
{
    std::vector<Entry> rotation;
    wchar_t wide[512] = {};
    GetPrivateProfileStringW(L"launch", L"Map", L"", wide, 512, ini);
    std::string list = Narrow(wide);
    const std::string fallback = DefaultMode(ini);
    char* next = nullptr;
    for (char* item = strtok_s(list.data(), ", ", &next); item; item = strtok_s(nullptr, ", ", &next))
    {
        Entry entry;
        if (ParseEntry(item, fallback, "[launch] Map", &entry))
            rotation.push_back(entry);
    }
    return rotation;
}

std::string FormatRotation(const std::vector<Entry>& rotation, const std::string& defaultMode)
{
    std::string text;
    for (const Entry& e : rotation)
    {
        if (!text.empty())
            text += ',';
        text += e.map;
        if (e.mode != defaultMode)
            text += ":" + e.mode;
    }
    return text;
}

bool WriteRotation(const wchar_t* ini, const std::vector<Entry>& rotation)
{
    std::string text = FormatRotation(rotation, DefaultMode(ini));
    wchar_t wide[512] = {};
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, wide, 512);
    return WritePrivateProfileStringW(L"launch", L"Map", wide, ini) != 0;
}

void SetCurrent(const std::string& map, const std::string& mode)
{
    g_launchMap = map;
    g_launchMode = mode.empty() ? "TeamDeathMatch" : mode;
}

const std::string& CurrentMode() { return g_launchMode; }
const std::string& CurrentMap() { return g_launchMap; }
bool IsTeamDeathmatch() { return g_launchMode == "TeamDeathMatch"; }
bool OneLifePerRound() { return g_launchMode == "Sport"; }

}  // namespace modes
