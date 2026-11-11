// Offline test of the restart for ProCon's Next Round (dll/src/relaunch.cpp): this exe stands in for the game. Started
// plainly it calls relaunch::Now(), which must start this exe again with no arguments and end this process; the copy
// calls relaunch::WaitForPrevious() and writes what it saw to relaunchtest.txt next to the exe, then exits.
// relaunchtest, then RELAUNCHTEST_WATCH=1 relaunchtest: the second prints what the copy wrote.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "../../dll/src/relaunch.h"

static wchar_t g_dir[MAX_PATH];
static std::string g_logged;

void Log(const char* fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    g_logged += std::string(text) + "\n";
}

const wchar_t* ModuleDir() { return g_dir; }

int wmain()
{
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    *(wcsrchr(g_dir, L'\\') + 1) = L'\0';
    std::wstring result = std::wstring(g_dir) + L"relaunchtest.txt";
    wchar_t pid[16] = {};
    if (GetEnvironmentVariableW(L"MOHW_BOTS_AFTER_PID", pid, 16))
    {
        // The copy: what the game's DllMain does first.
        relaunch::WaitForPrevious();
        wchar_t cwd[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        wchar_t left[16] = {};
        const bool cleared = !GetEnvironmentVariableW(L"MOHW_BOTS_AFTER_PID", left, 16);
        FILE* f = _wfopen(result.c_str(), L"w");
        std::fprintf(f, "command line: %ls\ncurrent folder: %ls\nvariable cleared: %s\n%s", GetCommandLineW(), cwd,
                     cleared ? "yes" : "NO", g_logged.c_str());
        std::fclose(f);
        return 0;
    }
    // A watcher (RELAUNCHTEST_WATCH set) waits for the copy's result, since the original ends inside Now().
    if (GetEnvironmentVariableW(L"RELAUNCHTEST_WATCH", pid, 16))
    {
        for (int i = 0; i < 100; ++i)
        {
            Sleep(100);
            FILE* f = _wfopen(result.c_str(), L"r");
            if (!f)
                continue;
            Sleep(200);
            char text[2048] = {};
            std::fread(text, 1, sizeof(text) - 1, f);
            std::fclose(f);
            std::printf("%s", text);
            return 0;
        }
        std::printf("FAILED: the copy never wrote its result\n");
        return 1;
    }
    DeleteFileW(result.c_str());
    std::printf("original (process %lu): restarting\n", GetCurrentProcessId());
    std::fflush(stdout);
    relaunch::Now();
    std::printf("FAILED: Now() returned: %s", g_logged.c_str());
    return 1;
}
