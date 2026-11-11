// XInput 1.3 proxy: the game imports ordinals 2-4 from xinput1_3.dll. A copy of this DLL in the game folder is
// loaded first; every export forwards to the real system DLL, loaded lazily (never from DllMain).
#include <windows.h>

#include "log.h"
#include "overrides.h"
#include "spotting.h"

namespace {

HMODULE RealXInput()
{
    static HMODULE real = nullptr;
    if (real)
        return real;
    const wchar_t* candidates[] = {L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"};
    wchar_t sysdir[MAX_PATH] = {};
    GetSystemDirectoryW(sysdir, MAX_PATH);  // 32-bit process: redirected to SysWOW64
    for (const wchar_t* name : candidates)
    {
        wchar_t path[MAX_PATH];
        wsprintfW(path, L"%s\\%s", sysdir, name);
        if ((real = LoadLibraryW(path)) != nullptr)
        {
            Log("xinput proxy: forwarding to %ls", path);
            return real;
        }
    }
    Log("xinput proxy: no system XInput DLL found; controllers will report disconnected");
    return nullptr;
}

template <class Fn>
Fn Real(const char* name)
{
    HMODULE h = RealXInput();
    return h ? reinterpret_cast<Fn>(GetProcAddress(h, name)) : nullptr;
}

}  // namespace

extern "C" {

DWORD WINAPI XInputGetState(DWORD user, void* state)
{
    ApplyOverrides();  // earliest regular main-thread callback the game gives us
    spotting::ClientTick();  // the client runs on this thread (its messages are dispatched here)
    static auto fn = Real<DWORD(WINAPI*)(DWORD, void*)>("XInputGetState");
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD user, void* vibration)
{
    static auto fn = Real<DWORD(WINAPI*)(DWORD, void*)>("XInputSetState");
    return fn ? fn(user, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, void* caps)
{
    ApplyOverrides();
    static auto fn = Real<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetCapabilities");
    return fn ? fn(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI XInputEnable(BOOL enable)
{
    static auto fn = Real<void(WINAPI*)(BOOL)>("XInputEnable");
    if (fn)
        fn(enable);
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture)
{
    static auto fn = Real<DWORD(WINAPI*)(DWORD, GUID*, GUID*)>("XInputGetDSoundAudioDeviceGuids");
    return fn ? fn(user, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD user, BYTE type, void* info)
{
    static auto fn = Real<DWORD(WINAPI*)(DWORD, BYTE, void*)>("XInputGetBatteryInformation");
    return fn ? fn(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD user, DWORD reserved, void* keystroke)
{
    static auto fn = Real<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetKeystroke");
    return fn ? fn(user, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetStateEx(DWORD user, void* state)  // undocumented ordinal 100
{
    static auto fn = Real<DWORD(WINAPI*)(DWORD, void*)>(reinterpret_cast<const char*>(100));
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

}  // extern "C"
