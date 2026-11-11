# MOHW LAN pack — malware triage

Date: 2026-10-22. Source: `E:\mohw_client.exe`, extracted (never executed) to `re/lanpack/`.
Method: Windows Defender scan (report-only), PE structure/import/section analysis with `tools/pe_triage.py`,
targeted disassembly of every risky import/API with `tools/xref.py`, `tools/import_calls.py`, `tools/table_uses.py`.

## Verdict

**No evidence of malicious code.** Every risky capability traced to a benign, expected purpose
for a LAN/emulator pack. Two caveats remain: the pack *does* contain DRM-bypass components, and
static analysis can't fully prove absence of behavior (see "Residual risk").

## Files

| File | SHA-256 | Defender | What it is |
|---|---|---|---|
| `mohw_client.exe` (E:\) | `487630d5a93eda007fb152cac44ff5074867b77a9c646f57cf3f0b03677e9c6c` | clean | Stock WinRAR ZIP self-extractor (2016 stub, `Software\WinRAR SFX`), no SFX script — extracts only, runs nothing. |
| `lanmohw.exe` = `MOHW.exe` (identical) | `c04b8b3d2e651b11d551b68d75e586589e372d8cffc9028d2f39ba34321f4cc9` | clean | Retail MOHW exe (build timestamp 2012-12-17) with Origin DRM stripped. |
| `dinput8.dll` | `1d77ddd29c58883e38079ae33d5fbe650854b99560ff9f2832fed4191d57abdc` | clean | Game-side hook loaded via the game's own `DINPUT8` import. BFPortable "dinput8_client" (PDB path). |
| `Core/winhttp.dll` | `ba0febbfab64d6935a4e2205e121256b6e6a99b43f00e2009626665e8aaedb3b` | clean | WinHTTP proxy targeting `ActivationUI.exe` — EA activation bypass. |
| `BFUlauncher.exe` | `1cc7901de492dfa358a4c2bba9c8819bafaff7bf662a1acce5d33a1634b6b530` | clean | Local Blaze/Origin emulator + game launcher. |
| `MOHW.par` | `1083f9d8d5604d796f1ed949877c567d78722e97455ac9991fa9883a71d37d9c` | — | 202-byte game param file (obfuscated, from 2012). |

## Findings per binary

### `lanmohw.exe`
- Sections match retail Frostbite 2 layout (`ctr`, `typeinfo`, `fieldinf` also exist in retail `bf3.exe`).
- Retail `bf3.exe` has encrypted code (`.text`/`ctr` entropy 8.00) and starts in a DRM loader section.
  `lanmohw.exe` has decrypted code (entropy 5.7/5.3) and its entry point is the standard MSVC CRT start
  (`call __security_init_cookie; jmp __tmainCRTStartup`).
- `.666` (RWX, 8 KB) is EA's original **Origin "Exe Loader"** (strings: `Activation.dll load failed.`,
  `Exe Loader`, TransGaming/Mac detection). **Nothing jumps into it** — the one rel32 "hit" was inside a
  `shr ecx, 0x17` instruction. It's dead DRM residue.
- EA Authenticode signature present but `HashMismatch` — expected for a decrypted exe.
- Imports are the normal game set (D3D11, XInput, WinMM, ws2_32, PunkBuster names, Battlelog/EA URLs).
- **Limit:** the decrypted `.text` can't be diffed against retail statically (retail is encrypted on disk),
  so small code patches (e.g. LAN tweaks) can't be ruled out this way.

### `dinput8.dll`
- Exports only `DirectInput8Create` (proxy). Embeds MinHook.
- `sendto`/`gethostbyname` imports are only read once to save originals before hooking them; `connect`
  is hooked by name. Purpose: rewrite the game's own connections — `gosredirector.ea.com` →
  `-blazeIp` / localhost, fake `/redirector/getServerInstance` response, ProtoSSL certificate-check bypass
  (`ParseCertificateHook`, `ProtoSSLUpdateRecvServerCert_x32_mohw`).
- `SetThreadContext` is MinHook's thread IP fix-up.
- Contains an HTTP error-report template to `bf4play.com/handlerrors.php` — **no code or data references it**
  (dead strings).
- Writes log files (`dinput8.log`, `BlazeLog.txt`, `Dump.txt`, `registry.txt`), reads `\Origin\bf3lan.txt`.
  No process creation, no registry writes, no persistence.

### `Core/winhttp.dll`
- Proxy exporting the WinHTTP API, built as `ActivationUI.WinhttpProxy_x32` — patches EA's activation UI.
- No network or process-creation imports. `Select * from %s` is a string comparison inside a hook (spoofs a query result).

### `BFUlauncher.exe`
- Runs a **local server** (socket/bind/listen/accept, OpenSSL) emulating EA endpoints (`/redirector`, `/licenses`, `/cms`).
- **No outbound path found:** no `connect` import; WinINet, `CreateRemoteThread`, `WriteProcessMemory`,
  `ShellExecute` etc. are resolved into a 331-entry runtime API table, but **only 6 entries are ever called**:
  `GetComputerNameA`, `GetUserNameA`, `GetCurrentHwProfileA`, `RtlGetVersion`, `RegOpenKeyExA`, `RegQueryValueExA`.
- Those six build a **machine fingerprint** (`MachineGuid`, `BIOSVersion`, Windows Update `SusClientId`,
  computer/user name, HW profile) — almost certainly a stable local player ID. It stays local as far as static analysis shows.
- **Persistent system changes it makes:**
  - writes game install keys (`SOFTWARE\[Wow6432Node\]Danger Close Games\Medal of Honor Warfighter`: Install Dir, GDFBinary, ClientPath, DisplayName, Locale)
  - registers URL protocols `mohwlan://`, `bf3lan://`, `bf4lan://`, `bfhlan://`, `bf1lan://` → `BFUlauncher.exe %1` (its own names; does **not** hijack `origin://`)
- `CreateProcessW` launches the game exe; `DeleteFileA` deletes only its own logs (`bf_global.log`, `bf_lsx.log`).

## Retail comparison (added 2026-10-22, after install)
Retail `F:\Medal of Honor Warfighter\MOHW.exe` (SHA-256 `c000c1a3ff79fbfb50c3d5430bdece03dac1704a2439322f7ecd4a48bb25416b`,
backed up to `I:\MOHW Modding\retail-backup\MOHW.exe`) is the same size as `lanmohw.exe`. Section-by-section:
- `.rdata`, `typeinfo`, `fieldinf`, `.tls`, `.rsrc`: **identical**.
- `.text`, `ctr`: differ entirely (retail is encrypted on disk, LAN is decrypted) — can't be compared.
- `.data`: 4 bytes — MSVC `__security_cookie` (retail holds the default `0xBB40E64E`, LAN a random runtime value).
- `.idata`: import table filled in; DRM section (`.666`): 52 bytes of loader runtime state.
- Conclusion: `lanmohw.exe` is a **memory dump of the retail exe** with the DRM loader bypassed; no data or resource tampering.
- Known `.text` patch (found 2026-10-23): `0x410f30` — the "launched in EA offline mode?" check (reads env
  `EALaunchOfflineMode`) is overwritten with `mov al, 1; ret`, so an argument-less launch always goes offline. Benign.

## Residual risk & recommendations
1. Static analysis can't prove absence of behavior. For the first run, block outbound traffic for
   `BFUlauncher.exe` and `lanmohw.exe` in Windows Firewall (LAN/localhost still works) and watch with
   TCPView/Procmon once. Or run it in a VM.
2. **Keep the retail `MOHW.exe`** before extracting the pack over the install (the pack overwrites it).
3. The pack is DRM-bypass software (decrypted exe + activation proxy). Fine for a game you own; just be aware.
