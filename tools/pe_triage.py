"""Static PE triage: headers, sections, imports/exports, TLS, suspicious APIs and strings.

Never executes the target. Usage: python -I pe_triage.py <file> [<file> ...]
"""
import math
import re
import sys
from collections import Counter

import pefile

SUSPICIOUS_APIS = {
    "injection": {"CreateRemoteThread", "CreateRemoteThreadEx", "WriteProcessMemory", "VirtualAllocEx",
                  "NtWriteVirtualMemory", "QueueUserAPC", "SetThreadContext", "NtUnmapViewOfSection",
                  "RtlCreateUserThread", "NtCreateThreadEx"},
    "process": {"CreateProcessA", "CreateProcessW", "WinExec", "ShellExecuteA", "ShellExecuteW",
                "ShellExecuteExA", "ShellExecuteExW", "OpenProcess", "CreateProcessAsUserW"},
    "network": {"InternetOpenA", "InternetOpenW", "InternetOpenUrlA", "InternetOpenUrlW", "URLDownloadToFileA",
                "URLDownloadToFileW", "WinHttpOpen", "WinHttpConnect", "WinHttpSendRequest", "HttpSendRequestA",
                "HttpSendRequestW", "socket", "connect", "WSAStartup", "send", "recv", "gethostbyname",
                "getaddrinfo", "InternetReadFile"},
    "persistence": {"RegSetValueExA", "RegSetValueExW", "RegCreateKeyExA", "RegCreateKeyExW",
                    "CreateServiceA", "CreateServiceW", "StartServiceA", "StartServiceW"},
    "hooking": {"SetWindowsHookExA", "SetWindowsHookExW", "VirtualProtect", "VirtualProtectEx",
                "LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW", "GetProcAddress"},
    "crypto": {"CryptEncrypt", "CryptDecrypt", "CryptAcquireContextA", "CryptAcquireContextW",
               "BCryptEncrypt", "BCryptDecrypt", "CryptGenKey"},
    "anti-debug": {"IsDebuggerPresent", "CheckRemoteDebuggerPresent", "NtQueryInformationProcess",
                   "OutputDebugStringA", "NtSetInformationThread"},
    "privilege": {"AdjustTokenPrivileges", "LookupPrivilegeValueA", "LookupPrivilegeValueW",
                  "OpenProcessToken"},
    "files": {"DeleteFileA", "DeleteFileW", "MoveFileExA", "MoveFileExW", "CopyFileA", "CopyFileW"},
}

STRING_PATTERNS = {
    "url": rb"(?:https?|ftp|wss?)://[\x21-\x7e]{4,200}",
    "ipv4": rb"(?<![\d.])(?:(?:25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|1?\d?\d)(?::\d{2,5})?(?![\d.])",
    "domain": rb"\b[a-zA-Z0-9-]{2,63}\.(?:com|net|org|ru|io|me|info|xyz|biz|cc|su|top|cn|de|uk|fr|pw|gg|tk)\b",
    "registry": rb"(?:HKEY_|SOFTWARE\\|Software\\|CurrentVersion\\Run)[\x20-\x7e]{2,120}",
    "shell": rb"(?i)(?:cmd\.exe|powershell|rundll32|regsvr32|schtasks|wscript|cscript|mshta|bitsadmin|certutil)[\x20-\x7e]{0,100}",
    "exe_dll": rb"[\x20-\x7e]{1,80}\.(?:exe|dll|sys|bat|ps1|vbs|zlo)\b",
}


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = Counter(data)
    return -sum(c / len(data) * math.log2(c / len(data)) for c in counts.values())


def utf16_strings(data: bytes, minlen=5):
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){%d,}" % minlen, data):
        yield m.group().decode("utf-16le")


def triage(path: str, max_strings: int = 40):
    pe = pefile.PE(path)
    data = pe.__data__
    print("=" * 100)
    print(path)
    fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
    print(f"  machine={hex(fh.Machine)} timestamp={fh.TimeDateStamp} ({pefile.time.strftime('%Y-%m-%d', pefile.time.gmtime(fh.TimeDateStamp))})"
          f" dll={pe.is_dll()} subsystem={oh.Subsystem} imagebase={hex(oh.ImageBase)}")
    ep = oh.AddressOfEntryPoint
    ep_sect = next((s.Name.rstrip(b"\0").decode(errors="replace") for s in pe.sections if s.contains_rva(ep)), "?")
    print(f"  entrypoint rva={hex(ep)} in section {ep_sect}")
    sec_dir = oh.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_SECURITY"]]
    print(f"  authenticode blob: {'present' if sec_dir.Size else 'none'}")
    end_of_sections = max(s.PointerToRawData + s.SizeOfRawData for s in pe.sections)
    if len(data) > end_of_sections:
        print(f"  OVERLAY: {len(data) - end_of_sections} bytes at {hex(end_of_sections)} magic={data[end_of_sections:end_of_sections+8]!r}")
    print("  sections:")
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode(errors="replace")
        flags = "".join(f for f, bit in (("R", 0x40000000), ("W", 0x80000000), ("X", 0x20000000)) if s.Characteristics & bit)
        print(f"    {name:10s} va={hex(s.VirtualAddress):>10s} vsize={hex(s.Misc_VirtualSize):>10s} raw={hex(s.SizeOfRawData):>10s} {flags:3s} entropy={entropy(s.get_data()):.2f}")

    found = {k: set() for k in SUSPICIOUS_APIS}
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        print("  imports:")
        for imp in pe.DIRECTORY_ENTRY_IMPORT:
            names = [i.name.decode() if i.name else f"#{i.ordinal}" for i in imp.imports]
            print(f"    {imp.dll.decode():28s} {len(names):4d}: {', '.join(names[:12])}{' ...' if len(names) > 12 else ''}")
            for n in names:
                for cat, apis in SUSPICIOUS_APIS.items():
                    if n in apis:
                        found[cat].add(n)
    else:
        print("  imports: NONE (packed or manually resolved)")
    if hasattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT"):
        for imp in pe.DIRECTORY_ENTRY_DELAY_IMPORT:
            print(f"    (delay) {imp.dll.decode()}: {[i.name.decode() for i in imp.imports if i.name][:12]}")
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        ex = pe.DIRECTORY_ENTRY_EXPORT
        names = [e.name.decode() if e.name else f"#{e.ordinal}" for e in ex.symbols]
        fwd = [e.forwarder.decode() for e in ex.symbols if e.forwarder]
        print(f"  exports ({len(names)}): {', '.join(names[:20])}")
        if fwd:
            print(f"  forwarders: {fwd[:10]}")
    if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
        tls = pe.DIRECTORY_ENTRY_TLS.struct
        print(f"  TLS: callbacks table va={hex(tls.AddressOfCallBacks)}")
    print("  suspicious API categories:")
    for cat, apis in found.items():
        if apis:
            print(f"    {cat:12s} {sorted(apis)}")

    print("  interesting strings:")
    for label, pat in STRING_PATTERNS.items():
        hits = list(dict.fromkeys(m.group().decode(errors="replace") for m in re.finditer(pat, data)))
        wide = [s for s in utf16_strings(data) if re.search(pat.decode(errors="ignore"), s)]
        hits += [f"(w) {s}" for s in dict.fromkeys(wide)]
        if hits:
            print(f"    [{label}] {len(hits)} unique")
            for h in hits[:max_strings]:
                print(f"       {h[:160]}")


if __name__ == "__main__":
    limit = 40
    args = sys.argv[1:]
    if args and args[0].startswith("--max="):
        limit = int(args.pop(0).split("=", 1)[1])
    for p in args:
        try:
            triage(p, limit)
        except pefile.PEFormatError as e:
            print("=" * 100, f"\n{p}: not a PE ({e})")
