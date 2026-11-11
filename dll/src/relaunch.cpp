#include "relaunch.h"

#include <windows.h>

#include <cwchar>
#include <string>

#include "log.h"

namespace relaunch {
namespace {

// The old game's process id, for the new one (inherited environment; cleared again so the game's own children don't get it).
constexpr const wchar_t* kAfterPid = L"MOHW_BOTS_AFTER_PID";

}  // namespace

void WaitForPrevious()
{
    wchar_t text[16] = {};
    if (!GetEnvironmentVariableW(kAfterPid, text, 16))
        return;
    SetEnvironmentVariableW(kAfterPid, nullptr);
    const DWORD pid = wcstoul(text, nullptr, 10);
    HANDLE old = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (!old)
    {
        Log("restart: started by the last game (process %lu), which has already closed", pid);
        return;
    }
    const DWORD start = GetTickCount();
    const DWORD result = WaitForSingleObject(old, 20000);
    CloseHandle(old);
    Log("restart: started by the last game (process %lu); %s after %lu ms", pid,
        result == WAIT_OBJECT_0 ? "it closed" : "it was STILL RUNNING", GetTickCount() - start);
}

bool Now()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\') + 1);
    wchar_t pid[16];
    swprintf(pid, 16, L"%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(kAfterPid, pid);
    // Just the exe, quoted: any argument makes the game hand off to the EA app instead of starting offline.
    std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process))
    {
        const DWORD error = GetLastError();
        SetEnvironmentVariableW(kAfterPid, nullptr);
        Log("restart: couldn't start %ls again (error %lu); this game plays on", exe, error);
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Log("restart: started the game again (process %lu); closing this one", process.dwProcessId);
    // Not ExitProcess: the game's own shutdown with its threads still running is the riskier way out. Everything of ours
    // is on disk already (the log flushes each line, routes were saved, the ini writes are synchronous).
    TerminateProcess(GetCurrentProcess(), 0);
    return true;
}

}  // namespace relaunch
