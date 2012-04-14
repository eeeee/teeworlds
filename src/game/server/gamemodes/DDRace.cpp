/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
/* Based on Race mod stuff and tweaked by GreYFoX@GTi and others to fit our DDRace needs. */
#include <engine/server.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/player.h>
#include <game/server/gamecontext.h>
#include "DDRace.h"
#include "gamemode.h"
#include "ctf.h"

CGameControllerDDRace::CGameControllerDDRace(class CGameContext *pGameServer) :
		IGameController(pGameServer), m_Teams(pGameServer)
{
	m_pGameType = GAME_NAME;
	m_pCTF = new CGameControllerCTF(pGameServer);

	InitTeleporter();
}

bool CGameControllerDDRace::OnEntity(int Index, vec2 Pos, int a, int b, int c)
{
	if (IGameController::OnEntity(Index, Pos, a, b, c)) return 1;
	if (m_pCTF->OnEntity(Index, Pos, a,b,c)) return 1;
	return 0;
}

int CGameControllerDDRace::OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	IGameController::OnCharacterDeath(pVictim, pKiller, Weapon);
	m_pCTF->OnCharacterDeath(pVictim, pKiller, Weapon);
	return 1;
}

void CGameControllerDDRace::Blocked(class CCharacter *pVictim, class CPlayer *pKiller)
{
	m_pCTF->OnCharacterDeath(pVictim, pKiller, 0);
}

CGameControllerDDRace::~CGameControllerDDRace()
{
	free(m_pCTF);
	// Nothing to clean
}

void CGameControllerDDRace::Tick()
{
	IGameController::Tick();
	m_pCTF->Tick();
}

void CGameControllerDDRace::Snap(int snp)
{
	//IGameController::Snap(snp);
	m_pCTF->Snap(snp);
}

void CGameControllerDDRace::InitTeleporter()
{
	if (!GameServer()->Collision()->Layers()->TeleLayer())
		return;
	int Width = GameServer()->Collision()->Layers()->TeleLayer()->m_Width;
	int Height = GameServer()->Collision()->Layers()->TeleLayer()->m_Height;

	for (int i = 0; i < Width * Height; i++)
	{
		if (GameServer()->Collision()->TeleLayer()[i].m_Number > 0)
		{
			if (GameServer()->Collision()->TeleLayer()[i].m_Type
					== TILE_TELEOUT)
			{
				m_TeleOuts[GameServer()->Collision()->TeleLayer()[i].m_Number
						- 1].push_back(
						vec2(i % Width * 32 + 16, i / Width * 32 + 16));
			}
			else if (GameServer()->Collision()->TeleLayer()[i].m_Type
					== TILE_TELECHECKOUT)
			{
				m_TeleCheckOuts[GameServer()->Collision()->TeleLayer()[i].m_Number
						- 1].push_back(
						vec2(i % Width * 32 + 16, i / Width * 32 + 16));
			}
		}
	}
}

