/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#ifndef DDRACE_H
#define DDRACE_H
#include <game/server/gamecontroller.h>
#include <game/server/teams.h>
#include <game/server/entities/door.h>

#include <vector>
#include <map>

class CGameControllerDDRace: public IGameController
{
public:
	IGameController* m_pCTF;

	CGameControllerDDRace(class CGameContext *pGameServer);
	~CGameControllerDDRace();

	CGameTeams m_Teams;

	std::map<int, std::vector<vec2> > m_TeleOuts;
	std::map<int, std::vector<vec2> > m_TeleCheckOuts;

	void InitTeleporter();
	virtual void Tick();
	virtual void Snap(int snp);
	virtual bool OnEntity(int Index, vec2 Pos, int a=0, int b=0, int c=0);
	virtual int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon);
	void Blocked(class CCharacter *pVictim, class CPlayer *pKiller);
};
#endif
