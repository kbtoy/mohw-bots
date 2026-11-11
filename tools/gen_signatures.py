"""Generate dll/src/signatures.inc: the first bytes of every engine function the DLL calls, read from lanmohw.exe.

The DLL compares these at runtime and refuses to touch the engine on any mismatch.
Usage: python -I gen_signatures.py <lanmohw.exe> <out.inc>
"""
import sys

import pefile

# name, address, bytes to verify
FUNCS = [
    ("Server_update", 0xAA8DE0, 10),
    ("ServerPlayerManager_createPlayer", 0xB43A10, 8),
    ("ServerPlayerManager_setPlayerName", 0xB3EB20, 8),
    ("ServerPlayer_setIsAIPlayer", 0xB2ADA0, 8),
    ("ServerPlayer_setTeam", 0xB2F2D0, 8),
    ("ServerPlayer_setSelectedSoldierAsset", 0xB2F320, 8),
    ("ServerPlayer_setInput", 0xAF56F0, 8),
    ("Teams_count", 0x7CEC40, 6),
    ("Teams_getTeamEntityData", 0x7D3CA0, 6),
    ("getPlayerLoadoutState", 0xE3FD70, 8),
    ("PlayerLoadoutState_setKitIndex", 0xE3E1A0, 8),
    ("EntryInput_ctor", 0x80EF00, 8),
    ("EntryInput_dtor", 0x8078D0, 6),
    ("EntryInput_setAuthoritativeAiming", 0x800780, 8),
    ("ServerPlayerForceRespawnMessage_ctor", 0x259F330, 8),
    ("Message_dtor", 0x57F250, 6),
    ("GameContext_dispatchMessage", 0x5928C0, 7),
    ("PlayerExtent_spawnAt", 0xE1E9D0, 8),
    # not a function: WinMain's `push "-webmode spoffline"; push edi; call appendCommandLine` (startup-args patch)
    ("Startup_pushOfflineArgs", 0x418182, 7),
    # not a function: Origin component init `mov al, [VeniceOnlineSettings+0x6C EnableOrigin]; mov [esi+0x160], al`
    ("Origin_readEnableOrigin", 0xDC4211, 9),
    # not a function: server backend choice `cmp byte [setup+0x11], 0; mov esi, ecx; jne -> mov edi, 3 (Backend_Local)`
    ("ServerBackend_select", 0xD257B9, 8),
    # MessageManager::dispatch(msg): sub esp, 0x10; mov eax, [0x2B351A4] (8 position-independent bytes, trace hook)
    ("MessageManager_dispatch", 0x5925C0, 8),
    ("SettingsManager_getContainer", 0x58AA60, 5),
    # ClientConnection vtable[5] (vtable 0x2793EC8): sends NetworkDisconnectedMessage(reason, text); push ecx/ebp/esi/
    # edi; mov edi, ecx; push 0x10 (8 position-independent bytes, trace hook)
    ("ClientConnection_onDisconnected", 0x873E80, 8),
    # transport peer disconnect(connection, code, const char* text), ret 0xC: sends the "disconnected, reason" packet
    # (type 0xE: 8-bit code + text) the other side turns into transport state 13; push ecx; push ebp; mov ebp, [esp+0xC]
    ("NetPeer_disconnect", 0x61FE90, 6),
    # not functions: transport connection update (0x61D670), states 1/2 (Send Challenge / Send Custom Challenge):
    # `cmp dword [esi+0xA0], 0x0F` = give up after 15 resends (0.5 s apart) -> state 9 -> peer's NoReply (16)
    ("Transport_challengeRetryLimit1", 0x61D6EA, 7),
    ("Transport_challengeRetryLimit2", 0x61D703, 7),
    # transport connection: notify the listener (+0xAC) when state +0x6C differs from the last notified (+0x68);
    # mov eax, ecx; mov ecx, [eax+0x6C]; cmp [eax+0x68], ecx (8 position-independent bytes, trace hook)
    ("TransportConnection_notifyState", 0x613830, 8),
    # not a function: ClientStartMultiplayerMessage handler (0x8A68C0) copying msg+0x45 into client+0x1F2:
    # xor eax, eax; cmp [edi+0x45], al; mov ecx, esi; setne al  (+0x1F2 -> connection mode 2 = in-process link)
    ("ClientStartMultiplayer_readLocalLink", 0x8A699A, 10),
    # eastl::vector<T*>::insert(pos, const T&) (thiscall, ret 8; grows the vector), used on ServerPlayer+0x524
    # (selected unlocks) by the NetworkPlayerSelectedUnlockAssetsMessage handler 0xB41B70
    ("UnlockVector_insert", 0x987A40, 8),
    # not a function: kill handler 0xE75030 reading the online stats object g_0x2B14928 without a null check:
    # mov edx, [0x2B14928]; mov esi, [edx+0x3C]  (null offline -> crash on the first kill)
    ("KillStats_readOnlineStats", 0xE75631, 9),
    # not a function: score/awards routine 0x169BB40 (from scoring 0xE7F420). It awards the score first (0x169AD40),
    # then loops over awards asking the online stats object (0xEA7A90): mov ecx, [0x2B14928] (null offline). Only
    # this loop read is guarded - skipping the whole routine (the first fix) also threw away every score award.
    ("Awards_readOnlineStats", 0x169BBCE, 6),
    # deploy-stats message handler (thiscall onMessage(msg), ret 4), uses g_0x2B14928 unchecked: sub esp, 0x84
    ("DeploymentStats_onMessage", 0x169C680, 6),
    # Bot default loadout: build a NetworkPlayerSelectedUnlockAssetsMessage (0x60 bytes) holding only the kit (+0x38)
    # and run the server's handler for it. With no appearance (+0x48) and no weapons (+0x4C) the handler sets the kit
    # (ServerPlayer+0xD0), applies the first weapon of every WeaponTable part (NetworkPlayerSelectedWeaponMessage ->
    # 0xB41A10), fills default appearance for the kit (0xB3E810) and calls itself with that. Cleanup as at 0xB41FEB.
    ("UnlockAssetsMessage_ctor", 0x25E5E30, 7),
    ("ServerPlayer_onSelectedUnlockAssets", 0xB41B70, 8),
    ("FbArray_dtor", 0x5634B0, 5),
    ("RefCounted_release", 0x564300, 6),
    ("NetworkMessage_dtor", 0x6284E0, 9),
    # ServerSoldierEntity (vtable 0x27BA820) +0x148 shouldSingleStep() and +0x14C singleStepEntry(entryIndex): copy the
    # entry's player input (+0x428), scale its deltaTime, apply (vfunc 0x150), CharacterPhysicsEntity::step, vfunc 0x154
    ("ServerSoldier_shouldSingleStep", 0xB64800, 8),
    ("ServerSoldier_singleStepEntry", 0xB64890, 9),
    # bool ServerPhysics::rayQuery(ident, mode, const Vec3* from, const Vec3* to, RayCastHit* hit, u32 flags, excluded)
    # (thiscall on *0x2AEAF88, ret 0x1C; true = blocked). Spawn visibility uses mode 1, flags 0x34.
    ("ServerPhysics_rayQuery", 0x7AC8E0, 8),
    # not a function: client death screen (0x17D50E0, from the "killed by" panel 0x17D57F0) reading the killer's
    # persistence record [player+0x160] straight after the lookup 0x9075D0, which returns null for bots
    ("DeathScreen_readKillerRank", 0x17D5123, 6),
    # not a function: client soldier-vs-teammate check 0x95ECC0 reading LevelData.AISystem (+0x4C) settings without a
    # null check when the teammate is an AI player: mov eax, [eax+0xC]; test eax, eax (MP levels have no AISystem)
    ("TeammateCheck_readAISystem", 0x95EDA6, 5),
    # score event message (StatEventMessage subclass, type 0x6182A896): ctor(player, other, supportFlag, points,
    # &sidRange, bucket, visibility, buddySid*, buddyPoints, buddyDistance, requestVO), ret 0x2C; deleting dtor
    ("ScoreEventMessage_ctor", 0xE1DBF0, 7),
    ("ScoreEventMessage_dtor", 0xE1DD40, 6),
    # cdecl getFireteamBuddy(player): the teammate whose player extent reports the same fireteam (vfunc +0x28)
    ("Player_getFireteamBuddy", 0xE3A8A0, 8),
    # getComponent<MOHWServerSpottingTargetComponent>(entity) (thiscall, 1 unused byte arg, ret 4)
    ("Entity_getSpottingTarget", 0xE02780, 8),
    # MOHWServerSpottingTargetComponent::addSpot(spotterPlayer, SpottedType) (thiscall, ret 8): spotted for the
    # type's time (data +0x60 active, +0x64 passive, +0x80 victim, +0x88 MQ9, +0x90 flash), refreshed per spotter
    ("SpottingTarget_addSpot", 0x169C220, 8),
    # VeniceServerPlayerExtent::setSquad(squad, team) (thiscall, ret 8): +0xF4 squad id (5 = none), leader, message
    ("PlayerExtent_setSquad", 0xE1C850, 8),
    # not a function: explosion spotting (0xE030A0, VeniceExplosionEntityData SpotTypeOnExplode / UnspotsOnExplode)
    # reading the damage giver's player team, mov ecx, [eax+0x38C], straight after getGiverPlayer (0xBAE090) - null
    # when the thrower's player is gone (a dead bot's player is destroyed)
    ("ExplosionSpot_readGiverTeam", 0xE031FD, 6),
    # client side (main thread): local ClientPlayer, ClientPlayerManager::getPlayer(id), ClientPlayer::getSoldier,
    # getComponent<MOHWClientSpottingTargetComponent>(entity), and its addSpot(spotterClientPlayer, SpottedType)
    ("Client_getLocalPlayer", 0xE83CC0, 8),
    ("ClientPlayerManager_getPlayer", 0x901750, 8),
    ("ClientPlayer_getSoldier", 0x903B80, 8),
    ("ClientEntity_getSpottingTarget", 0xDE88B0, 8),
    ("ClientSpottingTarget_addSpot", 0x16B5D90, 8),
    # MOHWIGHUDMinimapComp (client): the per-player icon pass (thiscall(dt, localSoldier)) and the icon update for one
    # soldier (thiscall(dt, ?, soldier, type 2 = enemy, flags x4), ret 0x20) - hooked for diagnostics
    ("Minimap_playerIconPass", 0x1786230, 8),
    ("Minimap_updateIcon", 0x17850A0, 8),
    # not a function: in that pass, after `call [soldier+0xB0 vtable +0x24]` (isAI, 0x9550B0): `je +0x0E; mov eax,
    # [esp+0x3C] (your team); cmp [esp+0x14] (its team), eax; jne skip` - an enemy AI soldier is never drawn (the
    # campaign's minimap hides enemy AI). The je becomes jmp so enemy bots are treated as enemy humans (shown when spotted)
    ("Minimap_skipEnemyAI", 0x178635B, 10),
    # not functions: server startup (0xAA97F0) creates the server's animation manager (0x970 bytes, ctor 0x68A7F0,
    # stored in ServerGameContext+0x18) only for game type "multiplayer": `call 0x7B90E0 (setup+0x73 == 0, i.e.
    # singleplayer); test al, al; jne skip` and `call 0x7B9100 (setup+0x74, coop); test al, al; jne skip` (skip =
    # 0xAAA078), then GameAnimationSettings.ServerEnable. Without it no ServerBoneCollisionComponent ever updates
    # (0xB6E3B0) - bot soldiers' hit shapes never follow them. The two jne become NOPs (ForceServerAnimation).
    ("ServerAnim_skipIfSinglePlayer", 0xAA9FCE, 8),
    ("ServerAnim_skipIfCoop", 0xAA9FDD, 8),
    # not a function: in the shared damage sum (0xD3CE00; server 0xE3CBB0 -> 0xD3F930 -> 0xD3F7E0, and your game's
    # 0xDCC8B0 the same way), `cmp byte ptr [giver+0x354], 0; je +0x2C` - for an AI shooter the damage is then x its
    # current weapon's GameAIWeaponData multiplier (soldier +0x130 interface vfunc +0x74, 0xB5ECB0):
    # FriendlyAIBulletDamageMultiplier (+0x10) on team 1, BulletDamageMultiplier (+0x0C) on team 2 - the campaign's
    # weaker enemies. The je becomes jmp so bot bullets do a player's damage (BotBulletsAsPlayers).
    ("Damage_aiWeaponMultiplier", 0xD3CF1B, 9),
    # BotHealthFromServer: bots on your game behave as a remote human's soldier does. Three sites:
    # 0x962DB0(victim, giver), called first by ClientSoldierEntity's onDamage (vtable 0x27A09B8 +0x104, 0x964380), says
    # whether your game handles a hit itself: for an AI or no giver on an AI victim, jne +0x0B returned a yes (al = the
    # client's [+0x38]+0xA). The jne goes to `xor al, al; ret 8` (0x962DD4) instead.
    ("ClientSoldier_handlesHit_aiVictim", 0x962E18, 9),
    # 0xDCFC20: the health states ask "AI soldier and client arbitration on?" and if so read the soldier's own estimate
    # (vtable +0xD8: server's health + unacknowledged hits of your game's) rather than the server's copy (module +0x5AC,
    # vtable +0x7C). Becomes `xor eax, eax; ret`: always the server's.
    ("ClientHealth_usesOwnEstimate", 0xDCFC20, 9),
    # 0xDCF250 (the soldier's ghost update) keeps the server's last-hit data (+0x30..+0x4F) off an AI soldier, which has
    # its own from your game's hits - which it no longer gets. jne over the copy -> nops.
    ("ClientHealth_ghostHitInfoSkipAI", 0xDCF4C2, 9),
    # Stealth melee on bots: 0x981870 (ClientMeleeComponent) accepts a human victim at once (je 0x9819A9), an AI one
    # only in a canned-melee state (0x7EDBD0). The je becomes jmp (StealthMeleeOnBots).
    ("ClientMelee_aiVictimCheck", 0x98197C, 12),
    # ServerMeleeManager (static object 0x2AF31B0): is this soldier in a stealth kill (thiscall(soldier), as the
    # game's AI brain asks at 0x1854F3B), and the kill's start (thiscall(attacker, victim), ret 8).
    ("ServerMelee_isInvolved", 0xB73D40, 9),
    ("ServerMelee_start", 0xB844D0, 9),
    # Your game shows a chat message (argument: the network message, text at +0, player id +0x10C, channel +0x118; ret 4,
    # ecx unused). Channel 4 needs no player and shows as ADMIN (0xF55397): what ProCon's admin.say becomes.
    ("ClientChat_show", 0x873290, 11),
    # The soldier's health multiplier (0xB67E00): `test bl, bl; je` twice, bl = not AI. The second skips
    # ServerSettings.HumanHealthMultiplier, the setting behind BF3's vars.soldierHealth (hardcore.cpp).
    ("ServerSoldier_healthHumanTest1", 0xB67E6B, 10),
    ("ServerSoldier_healthHumanTest2", 0xB67E8E, 8),
    # ServerSoldierEntity damage entry (vtable 0x27BA820 +0x114; thiscall(damageInfo, giverInfo, ?), ret 0xC): a hit
    # flagged client-arbitrable (damageInfo +0x6D bit 8) with SyncedGameSettings.AllowClientSideDamageArbitration on is
    # dropped when the shooter is AI (+0x354) or +0x424, applied (soldier +0x6E8 handler) otherwise. Hooked to count
    # what reaches it: push ebx; mov ebx, [esp+8] (5 position-independent bytes)
    ("ServerSoldier_onDamage", 0xB64FE0, 9),
    # ServerSoldierEntity::kill(viaEntry, suicide) (vtable 0x27BA820 +0x17C, thiscall, ret 8): a hit of the soldier's
    # whole health (damage type 0x33) through its +0x6E8 handler. The server's NetworkSuicideMessage handler (0xB42590;
    # what the deploy screen sends to change class while alive) calls [player+0x430]->kill(0, 1).
    ("ServerSoldier_kill", 0xB5E790, 8),
    # the server's listener for DamageArbitrationClientDamageMessage (thiscall onMessage(msg), ret 4): a client's hit
    # claim, +0x28 victim player id, +0x30 giver player id (ids index ServerPlayerManager +0xA4); applies it when
    # SyncedGameSettings.AllowClientSideDamageArbitration is on. Prologue push ebp; mov ebp, esp; and esp, -16
    ("ArbitrationClaim_onMessage", 0xE41590, 8),
    # Spawn-point property reads (thiscall(out, context), ret 8; out = {override, &field}), hooked so a bot's fall-back
    # spawn keeps to its own team's points: TacticalSpawnPointChooserEntityData.TeamFilter (+0x160) - the gate of the
    # chooser's team filter 0xEE1C90 - and AlternateSpawnEntityData.Team (+0x60). Prologues sub esp, 8; lea eax, [ecx+..]
    ("TacticalChooser_getTeamFilter", 0x26BC090, 10),
    ("AlternateSpawn_getTeam", 0x25D0770, 8),
    # not functions: the three soldier paths that delete an AI player once its soldier dies or unspawns (0xB61BC0,
    # 0xB6BE10, 0xB18AD0): after clearing the player's soldier, `cmp byte [player+0x354], 0; je skip` -> release its
    # assets (0xB42D40) and ServerPlayerManager::deletePlayer (0xB3AC90). Humans keep their player (KeepPlayers).
    ("AiDeath_deletePlayer1", 0xB61C08, 9),
    ("AiDeath_deletePlayer2", 0xB6BE4B, 9),
    ("AiDeath_deletePlayer3", 0xB18B0F, 9),
    # The scoring system's ServerPlayer listener handler (thiscall onMessage(msg), ret 4; listener vtable 0x281D8B4,
    # +4 = the scoring system): a record (0xE7C710) for a joining human only, dropped (0xE7A650) for a leaving human
    # only. Prologue mov eax, [esp+4]; push edi; mov edi, ecx (7 position-independent bytes, scoreboard hook)
    ("Scoring_onPlayerMessage", 0xE80F40, 10),
    ("Scoring_addPlayerRecord", 0xE7C710, 8),
    ("Scoring_removePlayerRecord", 0xE7A650, 8),
    # PlayerScore (server, 0x178 bytes, ctor 0xE23030): kills + 1 (+0x140, dirty 2) and set points (+0x148, dirty 8)
    ("PlayerScore_addKill", 0xE15A80, 8),
    ("PlayerScore_setPoints", 0xE15C00, 8),
    # PlayerScore match totals (+0x168 kills, +0x16C deaths, +0x170 points, +0x174 flags; dirty 0x400), what the
    # end-of-round screens show (your game's entry +0x18C..+0x198): add this round's values (the game does it on
    # ServerRoundSubRoundOverMessage via 0xE66F20, which TDM never sends) and clear
    ("PlayerScore_addToTotals", 0xE15D70, 8),
    ("PlayerScore_clearTotals", 0xE15DB0, 8),
    # client: cdecl localize(out, sid) -> out, a wide string {begin, end, capacity, allocator} (as the death screen
    # 0x17D5E05 uses it), and that string's release (thiscall)
    ("Localize", 0xEC0B50, 10),
    ("WideString_release", 0x5152E0, 9),
    # ServerCapturePointEntity ctor(creationInfo, CapturePointEntityData) ret 8 (vtable 0x281B450): sub esp, 0x14; push
    # ebx/ebp/esi/edi (7 position-independent bytes, objectives hook). Fields: +0xA8 owner team (setTeam 0xE3A590),
    # +0xAC the team whose flag is up, +0xC0 / +0xC4 capture progress and flag height (ghost bit 2, sent / 100)
    ("CapturePoint_ctor", 0xE450D0, 8),
    # its destructor (0xE45330, the deleting one, calls it first): push ebx/ebp/esi; mov esi, ecx; push edi (6 bytes)
    ("CapturePoint_dtor", 0xE442E0, 8),
    # In-place level change (levelchange.cpp), the way the server's NetworkRequestLoadLevelMessage handler (0xAA8900) does it:
    # a LevelSetup (built inside a NetworkRequestLoadLevelMessage, +0x38, as the client's 0xF62DB0 does: name, GameMode option,
    # difficulty), copied into a ServerLoadLevelMessage (0x48 bytes from the server arena 0x2A59130, base ctor 0x259EB00, vtable
    # 0x27AD710, setup +0x28, +0x40 = 0) queued 0.6 s ahead on the server context, then ServerRoundResetMessage dispatched.
    ("NetworkRequestLoadLevel_ctor", 0x258C490, 8),
    ("CString_assign", 0x527A10, 8),
    ("LevelSetup_setInclusionOption", 0x7E7C20, 8),
    ("LevelSetup_copy", 0x258FE50, 8),
    ("LevelSetup_dtor", 0x40A9D0, 8),
    ("ServerLoadLevelMessage_ctor", 0x259EB00, 8),
    ("GameContext_queueMessage", 0x588360, 8),
    ("ServerRoundResetMessage_ctor", 0x25A0780, 8),
    ("Arena_alloc", 0x54BA10, 6),
    # the server's handler for it (thiscall(server, msg)); only checked, its flags are logged: +0x40 loading, +0x41 unload
    # pending, +0x42, +0x6C same level, +0x6D load requested, +0x7C the current level
    ("Server_onLoadLevelMessage", 0xAAB140, 8),
    # deleting an AI player the way the game does once its soldier is gone (0xB61C11, 0xB6BE54): release its loadout
    # (thiscall(player): `mov byte [ecx+0x454], 0`, then a jmp to 0xB417C0), then ServerPlayerManager::deletePlayer
    # (thiscall(context+0x30 manager, player), ret 4), which takes it off the lists and frees it
    ("ServerPlayer_releaseLoadout", 0xB42D40, 7),
    ("ServerPlayerManager_deletePlayer", 0xB3AC90, 8),
    # client voice chat (cdecl(voipRef, mask)): VoipControl(ref, 'uspk' / 'umic', mask, 0) (0x1C1FE50), which reads
    # the ref unchecked. The ref is VoipGetRef() (0x1C2B120, [0x2B4447C]) - null offline, where voice never starts.
    # Both start with `mov eax, [esp+8]; mov ecx, [esp+4]` (8 position-independent bytes)
    ("Voip_setSpeakerMask", 0x1C20100, 8),
    ("Voip_setMicMask", 0x1C200E0, 8),
]


def main():
    pe = pefile.PE(sys.argv[1], fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    lines = ["// Generated by tools/gen_signatures.py from lanmohw.exe - do not edit by hand.",
             "// SIG(name, address, byte count, bytes...)"]
    for name, va, n in FUNCS:
        raw = img[va - base:va - base + n]
        lines.append(f"SIG({name}, 0x{va:08X}, {n}, " + ", ".join(f"0x{b:02X}" for b in raw) + ")")
    open(sys.argv[2], "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
