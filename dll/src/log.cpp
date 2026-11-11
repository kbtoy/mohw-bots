#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
FILE* g_file = nullptr;

}  // namespace

const wchar_t* ModuleDir()
{
    static wchar_t dir[MAX_PATH] = {};
    if (!dir[0])
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
        GetModuleFileNameW(self, dir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(dir, L'\\'))
            slash[1] = L'\0';
    }
    return dir;
}

void Log(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);

    AcquireSRWLockExclusive(&g_lock);
    if (!g_file)
    {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%smohw-bots.log", ModuleDir());
        g_file = _wfopen(path, L"a");
    }
    if (g_file)
    {
        fprintf(g_file, "%02d:%02d:%02d.%03d [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                GetCurrentThreadId(), line);
        fflush(g_file);
    }
    ReleaseSRWLockExclusive(&g_lock);
}
