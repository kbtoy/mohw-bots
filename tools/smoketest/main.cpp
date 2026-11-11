// Loads the bot DLL into a non-game 32-bit process: it must log, fail the signature check safely, and still
// forward XInput calls. Usage: smoketest.exe <path\to\xinput1_3.dll>
#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 2;
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll)
    {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    using GetState = DWORD(WINAPI*)(DWORD, void*);
    auto byOrdinal = reinterpret_cast<GetState>(GetProcAddress(dll, MAKEINTRESOURCEA(2)));
    unsigned char state[16] = {};
    DWORD r = byOrdinal ? byOrdinal(0, state) : 0xFFFFFFFF;
    std::printf("ordinal 2 (XInputGetState) -> %lu (0 = connected, 1167 = no controller)\n", r);
    return 0;
}
