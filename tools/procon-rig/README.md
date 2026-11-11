# ProCon page test

`gameplay-page-test.cs` hosts ProCon's own Server Settings > Gameplay page for MOHW (from `PRoCon.exe`) in a hidden form, connects it through the
real `PRoCon.Core` to `build/smoketest/Release/rcontest.exe`, flips its boxes the way a click does, logs what each box shows and saves screenshots.

Needs a copy of ProCon (not the one you play with) whose `Configs/procon.cfg` connects to `127.0.0.1` port 47201, has
`procon.private.options.runPluginsInSandbox False`, and has `package/CMohwBots.cs` in `Plugins/MOHW`. Build it there:

    csc -platform:x86 -out:harness3.exe -r:PRoCon.Core.dll -r:PRoCon.exe -r:System.Windows.Forms.dll -r:System.Drawing.dll gameplay-page-test.cs

Copy `PRoCon.exe.config` to `harness3.exe.config`. Start `rcontest 47201 - rcontest.log`, then `harness3 <screenshot folder>` from the ProCon copy.
Add `old` as a second argument, and set `RCONTEST_OLD=1` for rcontest, to act like a game build from before `vars.hardCore`.
The page is hooked up after ProCon has logged in, so the values ProCon asks for at login don't reach it; only later replies do.

# ProCon Maplist test

`maplist-test.cs` does the same with ProCon's Maplist tab (`PRoCon.Controls.Maplist.uscMaplist`): it adds Sector Control on Basilan and Home Run
on Sarajevo from the pool, tries Hot Spot on Chitrail (the test server's stand-in check doesn't know that map, so it comes back as an error),
makes row 4 the next map, removes row 1, refreshes, and logs the list after each step. rcontest's log shows the `mapList.persist` command each
change queues. Build it like the other one:

    csc -platform:x86 -out:maplist-test.exe -r:PRoCon.Core.dll -r:PRoCon.exe -r:System.Windows.Forms.dll -r:System.Drawing.dll maplist-test.cs

then copy `PRoCon.exe.config` to `maplist-test.exe.config`, start `rcontest 47201 - rcontest.log` and run `maplist-test <screenshot folder>`.
