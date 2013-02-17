/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <game/server/gamecontext.h>
#include <engine/shared/protocol.h>
#include <engine/shared/config.h>
#include <game/server/entities/character.h>
#include <game/server/entities/flag.h>

#include "mod.h"

CGameControllerMOD::CGameControllerMOD(class CGameContext *pGameServer)
: IGameController(pGameServer)
{
	// Exchange this to a string that identifies your game mode.
	// DM, TDM and CTF are reserved for teeworlds original modes.
	m_pGameType = "DDWar";
	m_NextBroadcastTick = 0;
	m_NextAnnounceTick = 0;
	m_GameFlags = GAMEFLAG_FLAGS; // GAMEFLAG_TEAMS makes it a two-team gamemode
	m_apFlags[0] = 0;
	m_apFlags[1] = 0;
}

void CGameControllerMOD::Tick()
{
	// this is the main part of the gamemode, this function is run every tick

	IGameController::Tick();

	if(GameServer()->m_World.m_ResetRequested || GameServer()->m_World.m_Paused)
		return;

	static int LastBounceTick[MAX_CLIENTS] = {0};
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CCharacter *pChr = GameServer()->GetPlayerChar(i);
		if (!pChr || LastBounceTick[i] + g_Config.m_SvBounceDelay > Server()->Tick())
			continue;
		if (GameServer()->Collision()->GetCollisionAt(pChr->m_Pos.x, pChr->m_Pos.y) == TILE_BOUNCE)
		{
			GameServer()->CreateExplosion(pChr->m_Pos, i, WEAPON_GRENADE, true);
			GameServer()->CreateSound(pChr->m_Pos, SOUND_GRENADE_EXPLODE);
			pChr->TakeDamage(vec2(g_Config.m_SvBounceXforce/10.0f, g_Config.m_SvBounceYforce/10.0f), 0, i, WEAPON_GRENADE);
			LastBounceTick[i] = Server()->Tick();
		}
	}

	if (*g_Config.m_SvAnnouncement && m_NextAnnounceTick <= Server()->Tick())
	{
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, g_Config.m_SvAnnouncement);
		m_NextAnnounceTick = Server()->Tick() + g_Config.m_SvAnnouncementInterval * 60 * Server()->TickSpeed();
	}

	if (m_NextBroadcastTick <= Server()->Tick())
	{
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			if (GameServer()->IsClientReady(i))
			{
				if (GameServer()->m_apPlayers[i]->m_PersonalBroadcastTick > Server()->Tick())
					GameServer()->SendBroadcast(GameServer()->m_apPlayers[i]->m_PersonalBroadcast, i);
				else
					if (*g_Config.m_SvBroadcast)
						GameServer()->SendBroadcast(g_Config.m_SvBroadcast, i);
			}
		}
		m_NextBroadcastTick = Server()->Tick() + (Server()->TickSpeed()<<1);
	}

	for(int fi = 0; fi < 2; fi++)
	{
		CFlag *F = m_apFlags[fi];

		if(!F)
			continue;

		int Col = GameServer()->Collision()->GetCollisionAt(F->m_Pos.x, F->m_Pos.y);
		if(((Col&CCollision::COLFLAG_DEATH) && Col<=7) || F->GameLayerClipped(F->m_Pos))
		{
			GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", "flag_return");
			GameServer()->CreateSoundGlobal(SOUND_CTF_RETURN);
			F->Reset();
			continue;
		}

		//
		if(F->m_pCarryingCharacter)
		{
			// update flag position
			F->m_Pos = F->m_pCarryingCharacter->m_Pos;
		}
		else
		{
			CCharacter *apCloseCCharacters[MAX_CLIENTS];
			int Num = GameServer()->m_World.FindEntities(F->m_Pos, CFlag::ms_PhysSize, (CEntity**)apCloseCCharacters, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
			for(int i = 0; i < Num; i++)
			{
				if(!apCloseCCharacters[i]->IsAlive() || apCloseCCharacters[i]->GetPlayer()->GetTeam() == TEAM_SPECTATORS || GameServer()->Collision()->IntersectLine(F->m_Pos, apCloseCCharacters[i]->m_Pos, NULL, NULL))
					continue;

				bool car = false;
				for(int ff = 0; ff < 2; ff++)
				{
					if (m_apFlags[ff]->m_pCarryingCharacter == apCloseCCharacters[i])
						car = true;
				}

				if (car)
					continue;

				// take the flag
				if(F->m_AtStand)
				{
					F->m_GrabTick = Server()->Tick();
				}

				F->m_AtStand = 0;
				F->m_pCarryingCharacter = apCloseCCharacters[i];
				F->m_pCarryingCharacter->GetPlayer()->m_Score += 1;
				F->m_pCarryingCharacter->GetPlayer()->SetTeam(i, false, false);

				char aBuf[256];
				char p1[256];
				str_format(aBuf, sizeof(aBuf), "flag_grab %s",
					GameServer()->GetPlayerIDTuple(F->m_pCarryingCharacter->GetPlayer()->GetCID(), p1, sizeof p1));
				GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

				for(int c = 0; c < MAX_CLIENTS; c++)
				{
					CPlayer *pPlayer = GameServer()->m_apPlayers[c];
					if(!pPlayer)
						continue;

					if(pPlayer->GetTeam() == TEAM_SPECTATORS && pPlayer->m_SpectatorID != SPEC_FREEVIEW && GameServer()->m_apPlayers[pPlayer->m_SpectatorID] && GameServer()->m_apPlayers[pPlayer->m_SpectatorID]->GetTeam() == fi)
						GameServer()->CreateSoundGlobal(SOUND_CTF_GRAB_EN, c);
					else if(pPlayer->GetTeam() == fi)
						GameServer()->CreateSoundGlobal(SOUND_CTF_GRAB_EN, c);
					else
						GameServer()->CreateSoundGlobal(SOUND_CTF_GRAB_PL, c);
				}
				break;
			}

			if(!F->m_pCarryingCharacter && !F->m_AtStand)
			{
			/*	if(Server()->Tick() > F->m_DropTick + Server()->TickSpeed()*30)
				{
					GameServer()->CreateSoundGlobal(SOUND_CTF_RETURN);
					F->Reset();
				}
				else
				*/
				{
					F->m_Vel.y += GameServer()->m_World.m_Core.m_Tuning.m_Gravity;
					GameServer()->Collision()->MoveBox(&F->m_Pos, &F->m_Vel, vec2(F->ms_PhysSize, F->ms_PhysSize), 0.5f);
				}
			}
		}
	}
}

bool CGameControllerMOD::OnEntity(int Index, vec2 Pos)
{
	if(IGameController::OnEntity(Index, Pos))
		return true;

	int Team = -1;
	if(Index == ENTITY_FLAGSTAND_RED) Team = TEAM_RED;
	if(Index == ENTITY_FLAGSTAND_BLUE) Team = TEAM_BLUE;
	if(Team == -1 || m_apFlags[Team])
		return false;

	CFlag *F = new CFlag(&GameServer()->m_World, Team);
	F->m_StandPos = Pos;
	F->m_Pos = Pos;
	m_apFlags[Team] = F;
	GameServer()->m_World.InsertEntity(F);
	return true;
}

int CGameControllerMOD::OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int WeaponID)
{
	IGameController::OnCharacterDeath(pVictim, pKiller, WeaponID);
	int HadFlag = 0;

	// drop flags
	for(int i = 0; i < 2; i++)
	{
		CFlag *F = m_apFlags[i];
		if(F && pKiller && pKiller->GetCharacter() && F->m_pCarryingCharacter == pKiller->GetCharacter())
			HadFlag |= 2;
		if(F && F->m_pCarryingCharacter == pVictim)
		{
			GameServer()->CreateSoundGlobal(SOUND_CTF_DROP);
			F->m_DropTick = Server()->Tick();
			F->m_pCarryingCharacter = 0;
			F->m_Vel = vec2(0,0);

			if(pKiller && pKiller->GetTeam() != pVictim->GetPlayer()->GetTeam())
				pKiller->m_Score++;

			HadFlag |= 1;
		}
	}

	return HadFlag;
}

void CGameControllerMOD::Snap(int SnappingClient)
{
	IGameController::Snap(SnappingClient);

	CNetObj_GameData *pGameDataObj = (CNetObj_GameData *)Server()->SnapNewItem(NETOBJTYPE_GAMEDATA, 0, sizeof(CNetObj_GameData));
	if(!pGameDataObj)
		return;

	if(m_apFlags[TEAM_RED])
	{
		if(m_apFlags[TEAM_RED]->m_AtStand)
			pGameDataObj->m_FlagCarrierRed = FLAG_ATSTAND;
		else if(m_apFlags[TEAM_RED]->m_pCarryingCharacter && m_apFlags[TEAM_RED]->m_pCarryingCharacter->GetPlayer())
		{
			pGameDataObj->m_FlagCarrierRed = m_apFlags[TEAM_RED]->m_pCarryingCharacter->GetPlayer()->GetCID();
			if (!Server()->Translate(pGameDataObj->m_FlagCarrierRed, SnappingClient))
				pGameDataObj->m_FlagCarrierRed = FLAG_MISSING;
		}
		else
			pGameDataObj->m_FlagCarrierRed = FLAG_TAKEN;
	}
	else
		pGameDataObj->m_FlagCarrierRed = FLAG_MISSING;

	if(m_apFlags[TEAM_BLUE])
	{
		if(m_apFlags[TEAM_BLUE]->m_AtStand)
			pGameDataObj->m_FlagCarrierBlue = FLAG_ATSTAND;
		else if(m_apFlags[TEAM_BLUE]->m_pCarryingCharacter && m_apFlags[TEAM_BLUE]->m_pCarryingCharacter->GetPlayer())
		{
			pGameDataObj->m_FlagCarrierBlue = m_apFlags[TEAM_BLUE]->m_pCarryingCharacter->GetPlayer()->GetCID();
			if (!Server()->Translate(pGameDataObj->m_FlagCarrierBlue, SnappingClient))
				pGameDataObj->m_FlagCarrierBlue = FLAG_MISSING;
		}
		else
			pGameDataObj->m_FlagCarrierBlue = FLAG_TAKEN;
	}
	else
		pGameDataObj->m_FlagCarrierBlue = FLAG_MISSING;
}

void CGameControllerMOD::SetFlagCarrier(int Flag, CCharacter* pCarrier)
{
	if (!m_apFlags[Flag])
		return;
	for (int i=0;i<2;i++)
		if (GetFlagCarrier(i) == pCarrier)
			return;
	m_apFlags[Flag]->m_pCarryingCharacter = pCarrier;
	if (g_Config.m_SvTakeFlagSound)
		GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE);
}
