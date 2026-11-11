#pragma once

// Appends a timestamped line to mohw-bots.log next to this DLL. Safe from any thread.
void Log(const char* fmt, ...);

// Directory containing this DLL (with trailing backslash); used for the log and the .ini.
const wchar_t* ModuleDir();
