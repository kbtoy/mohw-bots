# MOHW Bots

Offline AI players for Medal of Honor: Warfighter: 10 v 10 matches against bots in Team Deathmatch, Sector Control,
Combat Mission, Hot Spot and Home Run, with no server to set up. The bot logic is based on
[fun-bots](https://github.com/Joe91/fun-bots), the Battlefield 3 Venice Unleashed mod.

It is a DLL (`xinput1_3.dll`) the game loads from its folder. It targets one build of the MOHW LAN pack's `lanmohw.exe`
and checks the code it hooks first (`dll/src/signatures.inc`); with any other build the bots stay off.

## Layout

| Folder | What's in it |
|---|---|
| `dll/` | The DLL: `src/`, `CMakeLists.txt`, the shipped `mohw-bots.ini`, and `README.md`, the technical notes (how each part works, and what each test round found) |
| `package/` | What goes in a tester's zip besides the DLL: `README.txt` for testers, `reset-bot-routes.bat`, the ProCon plugin `CMohwBots.cs` |
| `tools/` | Python tools for reading the game (Frostbite 2 bundles, EBX, type info, disassembly helpers), `package.py`, and `smoketest/` (offline tests) |
| `re/` | Reverse-engineering notes: `RE_NOTES.md`, the LAN pack's malware triage, function labels and the message table |
| `PORTING_PLAN.md` | The original plan for the port |

Not in the repo (`.gitignore`): the game's own files (`re/lanpack/`, extracted `re/ebx/`, the generated `re/sdk/`), the
fun-bots clone (`refs/fun-bots`), build output (`build/`, `dist/`) and `tools/.venv/`.

## Building

Visual Studio 2022 and CMake, 32-bit like the game:

```
cmake -S dll -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

That gives `build/Release/xinput1_3.dll`. Copy it into the game folder next to `lanmohw.exe`, with the game closed.

## Tests

```
cmake -S tools/smoketest -B build/smoketest -G "Visual Studio 17 2022" -A Win32
cmake --build build/smoketest --config Release
```

- `imagetest <lanmohw.image.bin> <xinput1_3.dll>` loads the DLL against a copy of the game's code and checks the hooks,
  engine calls and structure offsets. Make the image with `python -I tools/dump_image.py <lanmohw.exe> build/lanmohw.image.bin`
  (needs `pefile`).
- `modestest` (map rotation and modes) and `navtest` (the route graph) need nothing else.
- `rcontest` is a stand-in game for trying ProCon against; `relaunchtest` checks the game restart. Both are run by hand.

## Releases

Set the version in `dll/src/version.h`, build, then `python tools/package.py [game folder]` writes `dist/mohw-bots-<version>.zip`
(the DLL, the ini, the learned routes from the game folder, and the files in `package/`). It stops if a text file in the
zip holds a drive path or this PC's user name.
