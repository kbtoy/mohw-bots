// Offline test of the map rotation (dll/src/modes.cpp): [launch] Map entries with and without a mode, a mode a map
// doesn't have, unknown maps, and writing the list back. Builds modes.cpp directly with stub logging; uses a temp ini.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "../../dll/src/modes.h"

static int g_failures = 0;
static wchar_t g_dir[MAX_PATH];

void Log(const char* fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    std::printf("  log: %s\n", text);
}

const wchar_t* ModuleDir() { return g_dir; }

static void Check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

static std::string Text(const std::vector<modes::Entry>& rotation)
{
    std::string s;
    for (const modes::Entry& e : rotation)
        s += (s.empty() ? "" : " ") + e.map + ":" + e.mode;
    return s;
}

int main()
{
    GetTempPathW(MAX_PATH, g_dir);
    wchar_t ini[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%smodestest.ini", g_dir);
    DeleteFileW(ini);

    WritePrivateProfileStringW(L"launch", L"Map", L"MP_05,MP_03:SectorControl, mp_12:sport,MP_05:Sport,MP_99,MP_10:bombsquad", ini);
    std::vector<modes::Entry> r = modes::ReadRotation(ini);
    std::printf("  read: %s\n", Text(r).c_str());
    Check(Text(r) == "MP_05:TeamDeathMatch MP_03:SectorControl MP_12:Sport MP_05:TeamDeathMatch MP_10:BombSquad",
          "entries keep their mode (any case), a mode the map lacks plays Team Deathmatch, unknown maps drop out");

    WritePrivateProfileStringW(L"launch", L"Mode", L"SectorControl", ini);
    WritePrivateProfileStringW(L"launch", L"Map", L"MP_03,MP_10:TeamDeathMatch", ini);
    r = modes::ReadRotation(ini);
    Check(Text(r) == "MP_03:SectorControl MP_10:TeamDeathMatch", "an entry without a mode plays [launch] Mode");

    Check(modes::WriteRotation(ini, {{"MP_10", "SectorControl"}, {"MP_12", "Sport"}, {"MP_05", "TeamDeathMatch"}}), "written");
    wchar_t back[256] = {};
    GetPrivateProfileStringW(L"launch", L"Map", L"", back, 256, ini);
    Check(std::wstring(back) == L"MP_10,MP_12:Sport,MP_05:TeamDeathMatch", "written back with modes, [launch] Mode's left out");
    Check(Text(modes::ReadRotation(ini)) == "MP_10:SectorControl MP_12:Sport MP_05:TeamDeathMatch", "and reads the same");

    Check(modes::MapHasMode("MP_03", "Sport") && !modes::MapHasMode("MP_05", "Sport") && !modes::MapHasMode("MP_12", "BombSquad") &&
              modes::MapHasMode("MP_10", "BombSquad") && !modes::MapHasMode("MP_99", "TeamDeathMatch"),
          "which maps have which modes");
    Check(std::string(modes::DisplayName("BombSquad")) == "Hot Spot" && std::string(modes::DisplayName("Sport")) == "Home Run",
          "the game's names for the modes");
    DeleteFileW(ini);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
