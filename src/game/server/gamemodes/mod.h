/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMEMODES_MOD_H
#define GAME_SERVER_GAMEMODES_MOD_H
#include <game/server/gamecontroller.h>
#include <game/server/entities/flag.h>

// you can subclass GAMECONTROLLER_CTF, GAMECONTROLLER_TDM etc if you want
// todo a modification with their base as well.
class CGameControllerMOD : public IGameController
{
private:
	int m_NextBroadcastTick;
	int m_NextAnnounceTick;
	int m_aLastBounceTick[MAX_CLIENTS];

	class CFlag *m_apFlags[2];
public:
	CGameControllerMOD(class CGameContext *pGameServer);
	virtual void Tick();
	// add more virtual functions here if you wish

	virtual bool OnEntity(int Index, vec2 Pos);
	virtual int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon);
	virtual void Snap(int SnappingClient);

	CCharacter* GetFlagCarrier(int Flag) { return m_apFlags[Flag] ? m_apFlags[Flag]->m_pCarryingCharacter : 0; }
	void SetFlagCarrier(int Flag, CCharacter* pCarrier);
};
#endif
