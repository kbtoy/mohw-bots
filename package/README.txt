MOHW Bots - test build {{VERSION}}
=================================

Offline AI players for Medal of Honor: Warfighter: a 10 v 10 Team Deathmatch against bots, with no server to set up.
The bot logic is based on fun-bots, the Battlefield 3 Venice Unleashed mod.

This is a test build. It changes the game while it runs, and so far it has only been played on one PC.


WHAT YOU NEED
-------------
1. Medal of Honor: Warfighter, installed.
2. The MOHW LAN pack applied to that install, so lanmohw.exe is in the game folder. The pack replaces MOHW.exe, so back
   that file up before applying it.

This zip contains no game files and no LAN pack.

Your lanmohw.exe must be this exact build:
    Size:    {{EXE_SIZE}} bytes
    SHA-256: {{EXE_SHA256}}

To check it, open PowerShell in the game folder and run:
    Get-FileHash lanmohw.exe

With any other build the bots stay off and the game runs as normal. mohw-bots.log then says
"not the lanmohw.exe build these bots target".


INSTALL
-------
Extract everything in this zip into your game folder, next to lanmohw.exe. It adds:
    xinput1_3.dll
    mohw-bots.ini
    mohw-bots-nav\      (routes the bots have learned)
    reset-bot-routes.bat
    MOHW-Bots-README.txt

If Windows asks to replace an xinput1_3.dll that's already there, keep a copy of the old one first.

The DLL is unsigned and hooks into the game, so some antivirus programs may flag it.


PLAY
----
Start lanmohw.exe directly, with no arguments. It loads straight into a Team Deathmatch round. Teams fill with bots on
their own, so you don't need to press anything.

When a round ends, chat says the next round comes in 30 seconds; after the results screen the next map loads without
the game closing (new; if that doesn't work, the game restarts with it). Set [launch] NextRoundSeconds in mohw-bots.ini
to change the wait, or 0 to stay on the results screen. ProCon's Next Round changes map at any time (see PROCON). The
maps take turns:
Novi Grad Warzone, Somalia Stronghold, Sarajevo Stadium, Basilan Aftermath. To play other game modes, see GAME MODES.
The game can't kick you for being idle or for team kills: you are the only player, so a kick ended the whole match
(it froze the game once). [launch] IdleKick=1 and TeamKillKick=1 turn them back on.

Keys you don't normally need:
    F6    add a bot on your team
    F7    add a bot in front of you
    F8    add a bot at a spawn point
    F9    pause or resume all bots (they stand still and hold fire)
    F11   write all players and bots to the log


GAME MODES
----------
Besides Team Deathmatch you can play Sector Control, Combat Mission, Hot Spot and Home Run. Pick them in ProCon's Maplist
tab (see PROCON), or in mohw-bots.ini: in [launch] Map, put the mode after the map with a colon, for example
    Map=MP_05,MP_03:SectorControl,MP_10:Sport
The names to use: TeamDeathMatch, SectorControl, CombatMission, BombSquad (Hot Spot), Sport (Home Run). Every map has the
first three. Novi Grad Warzone has no Home Run and Basilan Aftermath has no Hot Spot; an entry asking for one plays Team
Deathmatch instead.
    - Sector Control: the bots go for the flags their team doesn't hold, stay on a flag until it's theirs, and some go
      back to defend one when enemies come near. This now works on every map, including Shogore Valley, Tungawan Jungle
      and the expansion's Darra Gun Market and Chitral Compound, where the bots used to ignore the flags (new).
    - Combat Mission, Hot Spot and Home Run: the bots only fight. They don't plant or defuse bombs or carry the flag yet,
      so the objectives are yours. Arming or disarming a charge now scores (+200) and counts on the end-of-round screen,
      as do the charge going off, the objective falling and Home Run's flag (new).
    - Home Run: a bot that dies sits out the rest of the round, as you do.
These modes are new. Combat Mission and Hot Spot have had a few rounds with bots; bots going for Sector Control's flags
and Home Run haven't been played yet. The bots start each mode with the routes they learned on that map in Team Deathmatch.


HARDCORE MODE
-------------
A copy of Battlefield 3's Hardcore preset. To turn it on, open mohw-bots.ini, find [hardcore] and set Enabled=1. You can also
switch it from ProCon (see below).
    - Friendly fire is on. Bots hold fire while a teammate is in the way.
    - Everyone spawns with 60% health, bots included, and health doesn't come back.
    - The HUD is off except the minimap: no crosshair, health, ammo, kill feed or score.
    - No kill cam, no name tags, and spotted enemies only show on the minimap.
    - You can only spawn on your fireteam buddy if they lead the fireteam.
Each rule can be changed on its own in the same section. This mode is new and needs testing.


RELEARNING A MAP
----------------
If the bots have learned a map badly, make them start over. With the game closed, double-click reset-bot-routes.bat and
pick the map. The old routes are kept as a .bak file. On a reset map the bots are clumsy at first, then learn as people
walk around, so expect them to get lost for a round or two.
While playing with ProCon open, type this in its console tab instead: nav.reset
That resets the map you're on. To reset another one, name it: nav.reset somalia


WHAT TO LOOK OUT FOR
--------------------
- Crashes or freezes. Pressing Tab while dead used to close the game; that's fixed in this build.
- Sector Control: flags should now be taken, by you and the bots (new; nothing in this LAN build ran the capture, so the
  bots' DLL does). Standing on a flag lowers the other team's flag, then raises yours: alone, about 20 seconds for a
  free flag and 40 for one the other team holds, quicker with more of your team; it stops while both teams are on it.
  A flag left half raised sinks back and ends up nobody's. Please say whether the round's tickets and spawns change
  with the flags, and whether the bots move on to other flags once theirs is taken. On each pole the small blue / red
  banner is the capture moving; the big flag at the top is grey while nobody holds the point and shows the holding
  team's national flag once it's taken (new; choose them in mohw-bots.ini, [sectorcontrol]).
- Each mode should now show its own HUD (new): in Sector Control the team tickets and the flags' state; before, every
  mode showed the Team Deathmatch one. The capture progress bar is still missing.
- Bots never spawn in the enemy's starting base, even if their team takes the flag there (new).
- Bots on a flag stand and watch the ways in, moving to another spot now and then, and spread over the flags instead of
  crowding the one at their spawn.
- When the next map loads without the game closing, your points should start again from 0. They used to carry over from the
  last round (fixed in this build).
- Bodies that go limp but keep moving around, or bots that vanish when they die instead of falling. A recent fix should
  have ended the limp bodies.
- In the new game modes: whether the round starts and ends properly, whether bots capture Sector Control flags, and
  whether Home Run's rounds come and go with the bots sitting out after they die.
- Stealth kills: come up behind a bot and melee it. This should do the instant kill it does on a real player. It's new and
  hasn't been proven in play yet, so please try it and say what happened.
- Bots stuck on scenery or walking into water.
- Push a team back into its base and stay there: its bots should start coming in from other spots on the map.
- Bots that feel too easy or too hard.


SENDING A REPORT
----------------
Say what you saw and roughly when, and send this file from the game folder:
    mohw-bots.log

The log includes your in-game name and the path to your game folder. It keeps growing across sessions, so you can delete
it between tests if it gets large.

If bots behaved badly on a map, the matching file in mohw-bots-nav\ helps too.


KNOWN ISSUES
------------
- End-of-round screens are mostly empty.
- Downed bots can't be revived. Multiplayer has reviving switched off for everyone.
- Bots still get stuck sometimes, and they can bunch up in tight spots.
- Most of the play so far has been Team Deathmatch on the four maps above. The zip also has routes from a round or two on
  Hara Dunes, Al Fara Cliffside, Tungawan Jungle and Chitral Compound; on Shogore Valley and Darra Gun Market the bots start
  with no routes and learn as people walk around. Add any of them to [launch] Map to try them (MP_13, MP_16, MP_19, MP_21,
  MP_18, MP_20).
- In Combat Mission, Hot Spot and Home Run the bots don't play the objectives (see GAME MODES).
- Sector Control's flag captures are new and untested (see WHAT TO LOOK OUT FOR).


SETTINGS
--------
mohw-bots.ini has comments for every setting. The ones you're most likely to change:
    [brain]  SkillPercent    how good the bots are (default 40)
    [bots]   TeamSize        players per team (default 10)
    [launch] Map             which maps take turns


PROCON (OPTIONAL)
-----------------
This part is new. Add a server in ProCon with host 127.0.0.1 and port 47200; any password works. It should show the players,
scores and kills, and let you kill or kick bots and send chat.
The Maplist tab is the map rotation ([launch] Map in mohw-bots.ini), modes included. Pick a map and mode in the list on the
right and add it, remove entries, or right-click one and choose to make it the next map. Each change is saved to
mohw-bots.ini. ProCon only offers the modes each map has.
To change map now, use Next Round at the top of ProCon's server page, or double-click an entry in the Maplist tab to go
straight to it. Restart Round plays the current map and mode again. The game loads the new map without closing (new;
the first try froze, which this build should fix): chat tells you what's coming, the bots all drop dead and leave, and
you get the loading screen and the deploy screen of the new map. The bots come back a few seconds after it's in, with
the same names and teams. If the new map hasn't come in after a couple of minutes, the game closes and starts again
with it instead, and ProCon reconnects by itself within about 20 seconds. To always restart the game instead, set
[rcon] NextRoundInPlace=0 in mohw-bots.ini.
Most of Server Settings > Gameplay works: Friendly fire, Killcam, Regenerate Health, Buddy Outline, the HUD boxes, GameMode
Counter, Bullet Damage, Soldier Health and Player respawn time change the game straight away. Team balance, Third person
vehicle cameras and the two player counts have nothing behind them in a bot match, so ProCon shows them failing.
Changes made there aren't saved for the next launch.
Only your own PC can connect unless you change [rcon] Bind in mohw-bots.ini. If you do, set a Password there too.

For Hardcore in ProCon, copy procon-plugin\CMohwBots.cs from this zip into ProCon's Plugins\MOHW folder and restart ProCon.
Then open the Plugins tab and tick MOHW Bots to enable it. ProCon greys out the Hardcore box on the Gameplay page for
Warfighter; the plugin makes it work. Tick it there, or set Hardcore to Yes or No on the plugin. The server name shows
[Hardcore] while it's on, and the boxes Hardcore changes update a few seconds later. Soldier health changes the next time each
soldier spawns, and the choice is saved for the next launch. You can still change single settings on the Gameplay page while
Hardcore is on; they stay as you set them until Hardcore is switched again. The Gameplay Presets list (Normal, Immersive) is
ProCon's own and doesn't touch the Hardcore box.
If no plugins show and ProCon's plugin console says "Inheritance security rules violated", its plugin sandbox can't start on
your PC. In ProCon's Options, on the Plugins tab, set Plugin security to "Run plugins with no restrictions" and restart it. That
lets every plugin in ProCon's folder do anything on your PC, so only do it if you trust the ones there.
Without the plugin, type this in ProCon's console tab: vars.preset hardcore  (or vars.preset normal)


REMOVING IT
-----------
Delete these from the game folder:
    xinput1_3.dll
    mohw-bots.ini
    mohw-bots-state.ini
    mohw-bots.log
    mohw-bots-nav\
    reset-bot-routes.bat
    MOHW-Bots-README.txt

If you kept an old xinput1_3.dll, put it back. Remove the bots before using this game folder for anything else, because the
DLL loads whenever the game starts.
