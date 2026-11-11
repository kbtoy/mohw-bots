#pragma once

// Hooks MessageManager::dispatch (0x5925C0): feeds every message to the offline scoring (scoring.cpp) and the bot
// spotting (spotting.cpp), and with `trace` logs the first time each message type is dispatched (time since start,
// thread, manager, category, name from messages.inc; shows where a load or join stops) plus per-minute counts and the
// disconnect hooks.
bool InstallMessageHook(bool trace);
