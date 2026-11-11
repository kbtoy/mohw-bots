#include "trace.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine.h"
#include "hook.h"
#include "log.h"
#include "bots.h"
#include "combatlog.h"
#include "grenades.h"
#include "scoreboard.h"
#include "rcon.h"
#include "scoring.h"
#include "spotting.h"

namespace {

struct MessageName
{
    uint32_t hash;
    const char* name;
};

const MessageName kNames[] = {
#define MSG(hash, name) {hash, name},
#include "messages.inc"
#undef MSG
};

const char* NameOf(uint32_t hash)
{
    size_t lo = 0;
    size_t hi = sizeof(kNames) / sizeof(kNames[0]);
    while (lo < hi)  // messages.inc is sorted by hash
    {
        size_t mid = (lo + hi) / 2;
        if (kNames[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < sizeof(kNames) / sizeof(kNames[0]) && kNames[lo].hash == hash ? kNames[lo].name : nullptr;
}

// Lock-free "seen" set plus per-type counters: dispatch runs on several threads and very often, so the hot path is
// one probe and one interlocked increment.
constexpr uint32_t kSlots = 4096;
volatile LONG g_seen[kSlots];
volatile LONG g_counts[kSlots];

// Counts one dispatch of `hash`; true the first time this type is seen.
bool Record(uint32_t hash)
{
    if (!hash)
        return false;
    uint32_t i = (hash * 2654435761u) >> 20;
    for (uint32_t probes = 0; probes < kSlots; ++probes, i = (i + 1) & (kSlots - 1))
    {
        LONG current = g_seen[i];
        if (current == static_cast<LONG>(hash))
        {
            InterlockedIncrement(&g_counts[i]);
            return false;
        }
        if (current == 0)
        {
            LONG previous = InterlockedCompareExchange(&g_seen[i], static_cast<LONG>(hash), 0);
            if (previous == 0 || previous == static_cast<LONG>(hash))
            {
                InterlockedIncrement(&g_counts[i]);
                return previous == 0;
            }
        }
    }
    return false;
}

// Once a minute: the message types dispatched in that minute, most frequent first (shows whether e.g. score and
// support-action messages keep flowing, which the first-occurrence log can't).
volatile LONG g_lastCountsLog = 0;

void LogCounts()
{
    struct Entry
    {
        uint32_t hash;
        LONG count;
    };
    static Entry entries[kSlots];  // only one thread at a time gets here (see MaybeLogCounts)
    size_t n = 0;
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        LONG count = InterlockedExchange(&g_counts[i], 0);
        if (count > 0)
            entries[n++] = {static_cast<uint32_t>(g_seen[i]), count};
    }
    std::sort(entries, entries + n, [](const Entry& a, const Entry& b) { return a.count > b.count; });
    Log("message counts, last minute (%u types):", static_cast<unsigned>(n));
    char line[600];
    size_t used = 0;
    for (size_t k = 0; k < n; ++k)
    {
        const char* name = NameOf(entries[k].hash);
        char item[96];
        if (name)
            std::snprintf(item, sizeof(item), "%s %ld", name, entries[k].count);
        else
            std::snprintf(item, sizeof(item), "%08X %ld", entries[k].hash, entries[k].count);
        if (used + std::strlen(item) + 3 > 220)
        {
            Log("  %s", line);
            used = 0;
        }
        used += std::snprintf(line + used, sizeof(line) - used, "%s%s", used ? ", " : "", item);
    }
    if (used)
        Log("  %s", line);
}

void MaybeLogCounts()
{
    LONG now = static_cast<LONG>(GetTickCount());
    LONG last = g_lastCountsLog;
    if (now - last < 60000)
        return;
    if (InterlockedCompareExchange(&g_lastCountsLog, now, last) == last)
        LogCounts();
}

using DispatchFn = void(__thiscall*)(void* manager, void* message);
DispatchFn g_original = nullptr;
DWORD g_startTicks = 0;

constexpr uint32_t kNetworkDisconnectedMessage = 0x2E6D10B1;  // +0x28 connection, +0x2C reason (char*)
constexpr uint32_t kUIExitToMenuReasonMessage = 0xE4D79B1E;   // +0x28 reason enum (assumed)
constexpr uint32_t kClientStartMultiplayerMessage = 0xBF585A6D;  // +0x28 address string, +0x44/+0x45 local flags

// Copies a NUL-terminated string from engine memory without trusting the pointer.
void ReadText(const void* source, char* out, size_t size)
{
    size_t i = 0;
    for (; i + 1 < size; ++i)
    {
        uint8_t c = 0;
        if (!eng::ProbeByte(static_cast<const char*>(source) + i, &c) || !c)
            break;
        out[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    out[i] = '\0';
}

// Payload details for the messages that explain a disconnect; logged every time, not just the first.
void LogDetails(const void* message, uint32_t type)
{
    const uint32_t* words = static_cast<const uint32_t*>(message);
    if (type == kNetworkDisconnectedMessage)
    {
        char reason[96] = "?";
        void* text = nullptr;
        if (eng::ProbePtr(&words[0x2C / 4], &text) && text)
            ReadText(text, reason, sizeof(reason));
        Log("msg detail: NetworkDisconnectedMessage connection %08X reason \"%s\"", words[0x28 / 4], reason);
    }
    else if (type == kClientStartMultiplayerMessage)
    {
        // +0x44 -> client+0x1F1 (connection mode 0), +0x45 -> client+0x1F2 (mode 2); neither -> mode 1 (UDP).
        char address[96] = "?";
        void* text = nullptr;
        if (eng::ProbePtr(&words[0x28 / 4], &text) && text)
            ReadText(text, address, sizeof(address));
        // +0x28 is a LevelSetup: +0x00 Name, +0x04 Array<LevelSetupOption {Criterion, Value}> InclusionOptions.
        char options[256] = "";
        size_t used = 0;
        void* elements = nullptr;
        void* count = nullptr;
        if (eng::ProbePtr(&words[0x2C / 4], &elements) && elements &&
            eng::ProbePtr(static_cast<char*>(elements) - 4, &count))
        {
            uint32_t n = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(count));
            for (uint32_t i = 0; i < n && i < 8 && used + 80 < sizeof(options); ++i)
            {
                void* criterion = nullptr;
                void* value = nullptr;
                char c[40] = "?";
                char v[40] = "?";
                if (eng::ProbePtr(static_cast<char*>(elements) + i * 8, &criterion) && criterion)
                    ReadText(criterion, c, sizeof(c));
                if (eng::ProbePtr(static_cast<char*>(elements) + i * 8 + 4, &value) && value)
                    ReadText(value, v, sizeof(v));
                used += std::snprintf(options + used, sizeof(options) - used, "%s%s=%s", i ? ";" : "", c, v);
            }
        }
        const uint8_t* bytes = static_cast<const uint8_t*>(message);
        Log("msg detail: ClientStartMultiplayerMessage level \"%s\" inclusion \"%s\" flags +0x44=%u +0x45=%u",
            address, options, bytes[0x44], bytes[0x45]);
    }
    else if (type == kUIExitToMenuReasonMessage)
    {
        // Re-sent every frame while the menu is up, so only changes are logged.
        static volatile LONG lastReason = -1;
        LONG reason = static_cast<LONG>(words[0x28 / 4]);
        if (InterlockedExchange(&lastReason, reason) != reason)
            Log("msg detail: UIExitToMenuReasonMessage +0x28 = %u (+0x2C = %u)", words[0x28 / 4], words[0x2C / 4]);
    }
}

// --- ClientConnection::onDisconnected backtrace ----------------------------------------------------------------
// Logs code addresses found on the stack from `stack` upward that look like return addresses.
void LogReturnAddresses(const uintptr_t* stack)
{
    int found = 0;
    for (int i = 0; i < 1024 && found < 20; ++i)
    {
        void* value = nullptr;
        if (!eng::ProbePtr(stack + i, &value))
            break;
        uintptr_t va = reinterpret_cast<uintptr_t>(value);
        if (eng::LooksLikeReturnAddress(va))
        {
            Log("  [esp+%03X] %08X", i * 4, va);
            ++found;
        }
    }
}

using DisconnectFn = void(__thiscall*)(void* connection, uint32_t reason, const void* text);
DisconnectFn g_originalDisconnect = nullptr;

// vtable[5] of the client connection: (reason code, eastl string range {begin, end}) -> NetworkDisconnectedMessage.
void __fastcall DisconnectDetour(void* connection, void* /*edx*/, uint32_t reason, const void* text)
{
    char reasonText[96] = "?";
    void* begin = nullptr;
    void* end = nullptr;
    if (text && eng::ProbePtr(text, &begin) && eng::ProbePtr(static_cast<const char*>(text) + 4, &end) && begin)
    {
        size_t length = end > begin ? static_cast<size_t>(static_cast<char*>(end) - static_cast<char*>(begin)) : 0;
        ReadText(begin, reasonText, length + 1 < sizeof(reasonText) ? length + 1 : sizeof(reasonText));
    }
    void* state = nullptr;
    eng::ProbePtr(static_cast<const char*>(connection) + 0xA0, &state);
    Log("disconnect: ClientConnection %p onDisconnected(reason %u, \"%s\"), connection state %u; return addresses:",
        connection, reason, reasonText, reinterpret_cast<uintptr_t>(state));
    LogReturnAddresses(static_cast<const uintptr_t*>(_AddressOfReturnAddress()));
    g_originalDisconnect(connection, reason, text);
}

// --- Transport connection state changes -----------------------------------------------------------------------
// Names from the game's own table (0x613990).
const char* TransportStateName(uint32_t state)
{
    static const char* const kStates[] = {"Idle", "Send Challenge", "Send Custom Challenge", "Send Connect",
                                          "Send Accept", "Send Accept Confirmation", "Send Connection Confirmed",
                                          "Connected", "Challenge Sent - No reply", "Challenge Failed!",
                                          "Accept Failed!", "Connect Failed (11)", "12", "Disconnected by remote",
                                          "14", "ERROR"};
    return state < 16 ? kStates[state] : "?";
}

using NotifyStateFn = void(__thiscall*)(void* connection);
NotifyStateFn g_originalNotifyState = nullptr;

// Runs every update for every transport connection; only logs when the state (+0x6C) differs from the last one
// the listener (+0xAC) was told about (+0x68), i.e. on each transition.
void __fastcall NotifyStateDetour(void* connection, void* /*edx*/)
{
    const uint32_t* words = static_cast<const uint32_t*>(connection);
    uint32_t previous = words[0x68 / 4];
    uint32_t state = words[0x6C / 4];
    if (previous != state)
        Log("transport: connection %p (listener %08X) %s -> %s after %u resends", connection, words[0xAC / 4],
            TransportStateName(previous), TransportStateName(state), words[0xA0 / 4]);
    g_originalNotifyState(connection);
}

using PeerDisconnectFn = void(__thiscall*)(void* peer, void* connection, uint32_t code, const char* text);
PeerDisconnectFn g_originalPeerDisconnect = nullptr;

// Transport peer disconnect: sends the 8-bit reason (SecureReason) + text to the other side. The client's
// "disconnected by remote, reason 16" starts here on the server's side.
void __fastcall PeerDisconnectDetour(void* peer, void* /*edx*/, void* connection, uint32_t code, const char* text)
{
    char reasonText[96] = "";
    if (text)
        ReadText(text, reasonText, sizeof(reasonText));
    Log("disconnect: NetPeer %p disconnect(connection %p, code %u, \"%s\"); return addresses:", peer, connection,
        code & 0xFF, reasonText);
    LogReturnAddresses(static_cast<const uintptr_t*>(_AddressOfReturnAddress()));
    g_originalPeerDisconnect(peer, connection, code, text);
}

bool g_trace = false;

// FB2 message header: +0x04 category hash, +0x08 type hash.
void __fastcall DispatchDetour(void* manager, void* /*edx*/, void* message)
{
    const uint32_t* header = static_cast<const uint32_t*>(message);
    uint32_t type = header[2];
    if (g_trace)
    {
        if (type == kNetworkDisconnectedMessage || type == kUIExitToMenuReasonMessage ||
            type == kClientStartMultiplayerMessage)
            LogDetails(message, type);
        MaybeLogCounts();
        if (Record(type))
        {
            const char* name = NameOf(type);
            double seconds = (GetTickCount() - g_startTicks) / 1000.0;
            if (name)
                Log("msg +%7.2fs mgr %p cat %08X %s", seconds, manager, header[1], name);
            else
                Log("msg +%7.2fs mgr %p cat %08X type %08X (unnamed)", seconds, manager, header[1], type);
        }
    }
    scoring::OnMessage(message);
    spotting::OnMessage(message);
    combatlog::OnMessage(message);
    scoreboard::OnMessage(message);
    bots::OnMessage(message);
    grenades::OnMessage(message);
    rcon::OnMessage(message);
    spotting::OnDispatch(manager);  // your game's client messages: its main thread
    g_original(manager, message);
}

}  // namespace

bool InstallMessageHook(bool trace)
{
    g_trace = trace;
    void* target = reinterpret_cast<void*>(eng::Address("MessageManager_dispatch"));
    g_startTicks = GetTickCount();
    g_lastCountsLog = static_cast<LONG>(g_startTicks);
    void* trampoline = target ? InlineHook(target, 8, reinterpret_cast<const void*>(&DispatchDetour)) : nullptr;
    if (!trampoline)
    {
        Log("message hook: MessageManager::dispatch @ %p NOT hooked; offline scoring, bot spotting and the trace are off",
            target);
        return false;
    }
    g_original = reinterpret_cast<DispatchFn>(trampoline);
    Log("message hook: MessageManager::dispatch @ %p hooked (offline scoring, bot spotting%s)", target,
        trace ? ", trace: first occurrence of each message type and per-minute counts" : "");
    if (!trace)
        return true;

    void* disconnect = reinterpret_cast<void*>(eng::Address("ClientConnection_onDisconnected"));
    void* disconnectTrampoline =
        disconnect ? InlineHook(disconnect, 8, reinterpret_cast<const void*>(&DisconnectDetour)) : nullptr;
    if (disconnectTrampoline)
    {
        g_originalDisconnect = reinterpret_cast<DisconnectFn>(disconnectTrampoline);
        Log("trace: ClientConnection::onDisconnected @ %p hooked; logging reason and call stack", disconnect);
    }

    void* peerDisconnect = reinterpret_cast<void*>(eng::Address("NetPeer_disconnect"));
    void* peerTrampoline =
        peerDisconnect ? InlineHook(peerDisconnect, 6, reinterpret_cast<const void*>(&PeerDisconnectDetour)) : nullptr;
    if (peerTrampoline)
    {
        g_originalPeerDisconnect = reinterpret_cast<PeerDisconnectFn>(peerTrampoline);
        Log("trace: NetPeer::disconnect @ %p hooked; logging reason and call stack", peerDisconnect);
    }

    void* notify = reinterpret_cast<void*>(eng::Address("TransportConnection_notifyState"));
    void* notifyTrampoline =
        notify ? InlineHook(notify, 8, reinterpret_cast<const void*>(&NotifyStateDetour)) : nullptr;
    if (notifyTrampoline)
    {
        g_originalNotifyState = reinterpret_cast<NotifyStateFn>(notifyTrampoline);
        Log("trace: transport connection state changes @ %p hooked", notify);
    }
    return true;
}
