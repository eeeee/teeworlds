/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <new>
#include <engine/shared/config.h>
#include <game/server/gamecontext.h>
#include <game/mapitems.h>

#include "character.h"
#include "laser.h"
#include "projectile.h"
#include "loltext.h"
#include "../gamemodes/mod.h"

//input count
struct CInputCount
{
	int m_Presses;
	int m_Releases;
};

CInputCount CountInput(int Prev, int Cur)
{
	CInputCount c = {0, 0};
	Prev &= INPUT_STATE_MASK;
	Cur &= INPUT_STATE_MASK;
	int i = Prev;

	while(i != Cur)
	{
		i = (i+1)&INPUT_STATE_MASK;
		if(i&1)
			c.m_Presses++;
		else
			c.m_Releases++;
	}

	return c;
}


MACRO_ALLOC_POOL_ID_IMPL(CCharacter, MAX_CLIENTS)

// Character, "physical" player's part
CCharacter::CCharacter(CGameWorld *pWorld)
: CEntity(pWorld, CGameWorld::ENTTYPE_CHARACTER)
{
	m_ProximityRadius = ms_PhysSize;
	m_Health = 0;
	m_Armor = 0;
}

void CCharacter::Reset()
{
	Destroy();
}

bool CCharacter::Spawn(CPlayer *pPlayer, vec2 Pos)
{
	m_EmoteStop = -1;
	m_LastAction = -1;
	m_ActiveWeapon = WEAPON_GUN;
	m_LastWeapon = WEAPON_HAMMER;
	m_QueuedWeapon = -1;

	m_pPlayer = pPlayer;
	if (m_pPlayer)
		m_pPlayer->OverrideColors(-1);
	m_Pos = Pos;

	m_Core.Reset();
	m_Core.Init(&GameServer()->m_World.m_Core, GameServer()->Collision());
	m_Core.m_Pos = m_Pos;
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = &m_Core;

	m_ReckoningTick = 0;
	mem_zero(&m_SendCore, sizeof(m_SendCore));
	mem_zero(&m_ReckoningCore, sizeof(m_ReckoningCore));

	GameServer()->m_World.InsertEntity(this);
	m_Alive = true;

	GameServer()->m_pController->OnCharacterSpawn(this);
	
	m_DefEmote = EMOTE_NORMAL;
	m_DefEmoteReset = -1;

	m_HammerScore = 0;

	m_Helper = -1;
	NewState(BS_FREE);

	return true;
}

void CCharacter::Destroy()
{
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	m_Alive = false;
}

void CCharacter::SetWeapon(int W)
{
	if(W == m_ActiveWeapon)
		return;

	m_LastWeapon = m_ActiveWeapon;
	m_QueuedWeapon = -1;
	m_ActiveWeapon = W;
	GameServer()->CreateSound(m_Pos, SOUND_WEAPON_SWITCH);

	if(m_ActiveWeapon < -1 || m_ActiveWeapon >= NUM_WEAPONS)
		m_ActiveWeapon = -1;
}

bool CCharacter::IsGrounded()
{
	if(GameServer()->Collision()->CheckPoint(m_Pos.x+m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	if(GameServer()->Collision()->CheckPoint(m_Pos.x-m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	return false;
}


void CCharacter::HandleNinja()
{
	if(m_ActiveWeapon != WEAPON_NINJA)
		return;

	if ((Server()->Tick() - m_Ninja.m_ActivationTick) > (g_pData->m_Weapons.m_Ninja.m_Duration * Server()->TickSpeed() / 1000))
	{
		// time's up, return
		TakeNinja();
		return;
	}

	// force ninja Weapon
	SetWeapon(WEAPON_NINJA);

	m_Ninja.m_CurrentMoveTime--;

	if (m_Ninja.m_CurrentMoveTime == 0)
	{
		// reset velocity
		m_Core.m_Vel = m_Ninja.m_ActivationDir*m_Ninja.m_OldVelAmount;
	}

	if (m_Ninja.m_CurrentMoveTime > 0)
	{
		// Set velocity
		m_Core.m_Vel = m_Ninja.m_ActivationDir * g_pData->m_Weapons.m_Ninja.m_Velocity;
		vec2 OldPos = m_Pos;
		GameServer()->Collision()->MoveBox(&m_Core.m_Pos, &m_Core.m_Vel, vec2(m_ProximityRadius, m_ProximityRadius), 0.f);

		// reset velocity so the client doesn't predict stuff
		m_Core.m_Vel = vec2(0.f, 0.f);

		// check if we Hit anything along the way
		{
			CCharacter *aEnts[MAX_CLIENTS];
			vec2 Dir = m_Pos - OldPos;
			float Radius = m_ProximityRadius * 2.0f;
			vec2 Center = OldPos + Dir * 0.5f;
			int Num = GameServer()->m_World.FindEntities(Center, Radius, (CEntity**)aEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);

			for (int i = 0; i < Num; ++i)
			{
				if (aEnts[i] == this)
					continue;

				// make sure we haven't Hit this object before
				bool bAlreadyHit = false;
				for (int j = 0; j < m_NumObjectsHit; j++)
				{
					if (m_apHitObjects[j] == aEnts[i])
						bAlreadyHit = true;
				}
				if (bAlreadyHit)
					continue;

				// check so we are sufficiently close
				if (distance(aEnts[i]->m_Pos, m_Pos) > (m_ProximityRadius * 2.0f))
					continue;

				// Hit a player, give him damage and stuffs...
				GameServer()->CreateSound(aEnts[i]->m_Pos, SOUND_NINJA_HIT);
				// set his velocity to fast upward (for now)
				if(m_NumObjectsHit < 10)
					m_apHitObjects[m_NumObjectsHit++] = aEnts[i];

				aEnts[i]->TakeDamage(vec2(0, -10.0f), g_pData->m_Weapons.m_Ninja.m_pBase->m_Damage, m_pPlayer->GetCID(), WEAPON_NINJA);
			}
		}

		return;
	}

	return;
}


void CCharacter::DoWeaponSwitch()
{
	// make sure we can switch
	if(m_ReloadTimer != 0 || m_QueuedWeapon == -1 || m_aWeapons[WEAPON_NINJA].m_Got)
		return;

	// switch Weapon
	SetWeapon(m_QueuedWeapon);
}

void CCharacter::HandleWeaponSwitch()
{
	int NumWeps = 0;
	for(int i = 0; i < NUM_WEAPONS; ++i)
		if (m_aWeapons[i].m_Got)
			NumWeps++;

	if (!NumWeps)
		return;

	int WantedWeapon = m_ActiveWeapon;
	if(m_QueuedWeapon != -1)
		WantedWeapon = m_QueuedWeapon;

	// select Weapon
	int Next = CountInput(m_LatestPrevInput.m_NextWeapon, m_LatestInput.m_NextWeapon).m_Presses;
	int Prev = CountInput(m_LatestPrevInput.m_PrevWeapon, m_LatestInput.m_PrevWeapon).m_Presses;

	if(Next < 128) // make sure we only try sane stuff
	{
		while(Next) // Next Weapon selection
		{
			WantedWeapon = (WantedWeapon+1)%NUM_WEAPONS;
			if(m_aWeapons[WantedWeapon].m_Got)
				Next--;
		}
	}

	if(Prev < 128) // make sure we only try sane stuff
	{
		while(Prev) // Prev Weapon selection
		{
			WantedWeapon = (WantedWeapon-1)<0?NUM_WEAPONS-1:WantedWeapon-1;
			if(m_aWeapons[WantedWeapon].m_Got)
				Prev--;
		}
	}

	// Direct Weapon selection
	if(m_LatestInput.m_WantedWeapon)
		WantedWeapon = m_Input.m_WantedWeapon-1;

	// check for insane values
	if(WantedWeapon >= 0 && WantedWeapon < NUM_WEAPONS && WantedWeapon != m_ActiveWeapon && m_aWeapons[WantedWeapon].m_Got)
		m_QueuedWeapon = WantedWeapon;

	DoWeaponSwitch();
}

void CCharacter::FireWeapon()
{
	if(m_ReloadTimer != 0)
		return;

	if (m_ActiveWeapon == -1)
		return;

	DoWeaponSwitch();
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	bool FullAuto = false;
	if(m_ActiveWeapon == WEAPON_GRENADE || m_ActiveWeapon == WEAPON_SHOTGUN || m_ActiveWeapon == WEAPON_RIFLE)
		FullAuto = true;


	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;

	if(FullAuto && (m_LatestInput.m_Fire&1) && m_aWeapons[m_ActiveWeapon].m_Ammo)
		WillFire = true;
		
	if (!g_Config.m_SvNinja && m_ActiveWeapon == WEAPON_NINJA)
		WillFire = false;

	if(!WillFire)
		return;

	// check for ammo
	if(!m_aWeapons[m_ActiveWeapon].m_Ammo)
	{
		// 125ms is a magical limit of how fast a human can click
		m_ReloadTimer = 125 * Server()->TickSpeed() / 1000;
		GameServer()->CreateSound(m_Pos, SOUND_WEAPON_NOAMMO);
		return;
	}

	vec2 ProjStartPos = m_Pos+Direction*m_ProximityRadius*0.75f;

	switch(m_ActiveWeapon)
	{
		case WEAPON_HAMMER:
		{
			// reset objects Hit
			m_NumObjectsHit = 0;
			GameServer()->CreateSound(m_Pos, SOUND_HAMMER_FIRE);

			CCharacter *apEnts[MAX_CLIENTS];
			int Hits = 0;
			int Num = GameServer()->m_World.FindEntities(ProjStartPos, m_ProximityRadius*0.5f, (CEntity**)apEnts,
														MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
			m_HammerScore += g_Config.m_SvHammerPenalty;

			for (int i = 0; i < Num; ++i)
			{
				CCharacter *pTarget = apEnts[i];
				
				//for race mod or any other mod, which needs hammer hits through the wall remove second condition
				if ((pTarget == this) /* || GameServer()->Collision()->IntersectLine(ProjStartPos, pTarget->m_Pos, NULL, NULL) */)
					continue;

				// set his velocity to fast upward (for now)
				if(length(pTarget->m_Pos-ProjStartPos) > 0.0f)
					GameServer()->CreateHammerHit(pTarget->m_Pos-normalize(pTarget->m_Pos-ProjStartPos)*m_ProximityRadius*0.5f);
				else
					GameServer()->CreateHammerHit(ProjStartPos);

				vec2 Dir;
				if (length(pTarget->m_Pos - m_Pos) > 0.0f)
					Dir = normalize(pTarget->m_Pos - m_Pos);
				else
					Dir = vec2(0.f, -1.f);

				pTarget->TakeDamage(vec2(0.f, -1.f) + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f, g_pData->m_Weapons.m_Hammer.m_pBase->m_Damage,
					m_pPlayer->GetCID(), m_ActiveWeapon);

				int Col = GameServer()->Collision()->GetCollisionAt(pTarget->m_Pos.x, pTarget->m_Pos.y);
				if (Col != TILE_FREEZE && (Col < TILE_COLFRZ_FIRST || Col > TILE_COLFRZ_LAST))
					pTarget->m_Core.m_Frozen = 0;

				Hits++;
				pTarget->Interaction(m_pPlayer->GetCID());
			}

			// if we Hit anything, we have to wait for the reload
			if(Hits)
				m_ReloadTimer = Server()->TickSpeed()/3;

		} break;

		case WEAPON_GUN:
		{
			CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_GUN,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GunLifetime),
				1, 0, 0, -1, WEAPON_GUN);

			// pack the Projectile and send it to the client Directly
			CNetObj_Projectile p;
			pProj->FillInfo(&p);

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(1);
			for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
				Msg.AddInt(((int *)&p)[i]);

			Server()->SendMsg(&Msg, 0, m_pPlayer->GetCID());

			GameServer()->CreateSound(m_Pos, SOUND_GUN_FIRE);
		} break;

		case WEAPON_SHOTGUN:
		{
			int ShotSpread = 2;

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(ShotSpread*2+1);

			for(int i = -ShotSpread; i <= ShotSpread; ++i)
			{
				float Spreading[] = {-0.185f, -0.070f, 0, 0.070f, 0.185f};
				float a = GetAngle(Direction);
				a += Spreading[i+2];
				float v = 1-(absolute(i)/(float)ShotSpread);
				float Speed = mix((float)GameServer()->Tuning()->m_ShotgunSpeeddiff, 1.0f, v);
				CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_SHOTGUN,
					m_pPlayer->GetCID(),
					ProjStartPos,
					vec2(cosf(a), sinf(a))*Speed,
					(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_ShotgunLifetime),
					1, 0, 0, -1, WEAPON_SHOTGUN);

				// pack the Projectile and send it to the client Directly
				CNetObj_Projectile p;
				pProj->FillInfo(&p);

				for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
					Msg.AddInt(((int *)&p)[i]);
			}

			Server()->SendMsg(&Msg, 0,m_pPlayer->GetCID());

			GameServer()->CreateSound(m_Pos, SOUND_SHOTGUN_FIRE);
		} break;

		case WEAPON_GRENADE:
		{
			CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_GRENADE,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GrenadeLifetime),
				1, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);

			// pack the Projectile and send it to the client Directly
			CNetObj_Projectile p;
			pProj->FillInfo(&p);

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(1);
			for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
				Msg.AddInt(((int *)&p)[i]);
			Server()->SendMsg(&Msg, 0, m_pPlayer->GetCID());

			GameServer()->CreateSound(m_Pos, SOUND_GRENADE_FIRE);
		} break;

		case WEAPON_RIFLE:
		{
			new CLaser(GameWorld(), m_Pos, Direction, GameServer()->Tuning()->m_LaserReach, m_pPlayer->GetCID());
			GameServer()->CreateSound(m_Pos, SOUND_RIFLE_FIRE);
		} break;

		case WEAPON_NINJA:
		{
			// reset Hit objects
			m_NumObjectsHit = 0;

			m_Ninja.m_ActivationDir = Direction;
			m_Ninja.m_CurrentMoveTime = g_pData->m_Weapons.m_Ninja.m_Movetime * Server()->TickSpeed() / 1000;
			m_Ninja.m_OldVelAmount = length(m_Core.m_Vel);

			GameServer()->CreateSound(m_Pos, SOUND_NINJA_FIRE);
		} break;

	}

	m_AttackTick = Server()->Tick();
	
	if (!g_Config.m_SvUnlimitedAmmo && m_aWeapons[m_ActiveWeapon].m_Ammo > 0)
		m_aWeapons[m_ActiveWeapon].m_Ammo--;

	if(!m_ReloadTimer)
		m_ReloadTimer = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Firedelay * Server()->TickSpeed() / 1000;
}

void CCharacter::HandleWeapons()
{
	if (m_ActiveWeapon == -1)
		return;

	//ninja
	HandleNinja();

	// check reload timer
	if(m_ReloadTimer)
	{
		m_ReloadTimer--;
		return;
	}

	// fire Weapon, if wanted
	FireWeapon();

	// ammo regen
	int AmmoRegenTime = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Ammoregentime;
	if(AmmoRegenTime)
	{
		// If equipped and not active, regen ammo?
		if (m_ReloadTimer <= 0)
		{
			if (m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart < 0)
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = Server()->Tick();

			if ((Server()->Tick() - m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart) >= AmmoRegenTime * Server()->TickSpeed() / 1000)
			{
				// Add some ammo
				m_aWeapons[m_ActiveWeapon].m_Ammo = min(m_aWeapons[m_ActiveWeapon].m_Ammo + 1, 10);
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
			}
		}
		else
		{
			m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
		}
	}

	return;
}
void CCharacter::TakeWeapon(int Weapon)
{
	m_aWeapons[Weapon].m_Got = false;
	if (m_ActiveWeapon == Weapon)
	{
		int NewWeap = -1;
		if (m_LastWeapon != -1 && m_LastWeapon != Weapon && m_aWeapons[m_LastWeapon].m_Got)
			NewWeap = m_LastWeapon;
		else
		{
			for(NewWeap = 0; NewWeap < NUM_WEAPONS && !m_aWeapons[NewWeap].m_Got; NewWeap++);
			if (NewWeap == NUM_WEAPONS)
				NewWeap = -1;
		}
		SetWeapon(NewWeap);
		if (m_LastWeapon != -1 && !m_aWeapons[m_LastWeapon].m_Got)
			m_LastWeapon = m_ActiveWeapon;
	}
}

bool CCharacter::GiveWeapon(int Weapon, int Ammo)
{
	if(m_aWeapons[Weapon].m_Ammo < g_pData->m_Weapons.m_aId[Weapon].m_Maxammo || !m_aWeapons[Weapon].m_Got)
	{
		m_aWeapons[Weapon].m_Got = true;
		m_aWeapons[Weapon].m_Ammo = min(g_pData->m_Weapons.m_aId[Weapon].m_Maxammo, Ammo);
		return true;
	}
	return false;
}

void CCharacter::GiveNinja(bool Silent)
{
	m_Ninja.m_ActivationTick = Server()->Tick();
	m_aWeapons[WEAPON_NINJA].m_Got = true;
	m_aWeapons[WEAPON_NINJA].m_Ammo = -1;
	if (m_ActiveWeapon != WEAPON_NINJA)
		m_LastWeapon = m_ActiveWeapon;
	m_ActiveWeapon = WEAPON_NINJA;
	
	if (!Silent)
		GameServer()->CreateSound(m_Pos, SOUND_PICKUP_NINJA);
}

void CCharacter::TakeNinja()
{
	if (m_ActiveWeapon != WEAPON_NINJA)
		return;

	m_aWeapons[WEAPON_NINJA].m_Got = false;
	m_ActiveWeapon = m_LastWeapon;
	if(m_ActiveWeapon == WEAPON_NINJA)
		m_ActiveWeapon = WEAPON_HAMMER;
	//SetWeapon(m_ActiveWeapon); //has no effect
}

void CCharacter::SetDefEmote(int Emote, int Tick)
{
	m_DefEmote = Emote;
	m_DefEmoteReset = Tick;
}

void CCharacter::SetEmote(int Emote, int Tick)
{
	m_EmoteType = Emote;
	m_EmoteStop = Tick;
}

void CCharacter::OnPredictedInput(CNetObj_PlayerInput *pNewInput)
{
	// check for changes
	if(mem_comp(&m_Input, pNewInput, sizeof(CNetObj_PlayerInput)) != 0)
		m_LastAction = Server()->Tick();

	// copy new input
	mem_copy(&m_Input, pNewInput, sizeof(m_Input));
	m_NumInputs++;

	// it is not allowed to aim in the center
	if(m_Input.m_TargetX == 0 && m_Input.m_TargetY == 0)
		m_Input.m_TargetY = -1;
}

void CCharacter::OnDirectInput(CNetObj_PlayerInput *pNewInput)
{
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestInput, pNewInput, sizeof(m_LatestInput));

	// it is not allowed to aim in the center
	if(m_LatestInput.m_TargetX == 0 && m_LatestInput.m_TargetY == 0)
		m_LatestInput.m_TargetY = -1;

	if(m_NumInputs > 2 && m_pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		HandleWeaponSwitch();
		FireWeapon();
	}

	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
}

void CCharacter::ResetInput()
{
	m_Input.m_Direction = 0;
	m_Input.m_Hook = 0;
	// simulate releasing the fire button
	if((m_Input.m_Fire&1) != 0)
		m_Input.m_Fire++;
	m_Input.m_Fire &= INPUT_STATE_MASK;
	m_Input.m_Jump = 0;
	m_LatestPrevInput = m_LatestInput = m_Input;
}

void CCharacter::Tick()
{
	if(m_pPlayer->m_ForceBalanced)
	{
		char Buf[128];
		str_format(Buf, sizeof(Buf), "You were moved to %s due to team balancing", GameServer()->m_pController->GetTeamName(m_pPlayer->GetTeam()));
		GameServer()->SendBroadcast(Buf, m_pPlayer->GetCID());

		m_pPlayer->m_ForceBalanced = false;
	}

	m_Core.m_Input = m_Input;
	m_Core.Tick(true);
	
	if (m_Core.m_Frozen)
	{
		if (m_ActiveWeapon != WEAPON_NINJA)
			GiveNinja(true);
		else if (m_Ninja.m_ActivationTick + 5 * Server()->TickSpeed() < Server()->Tick())
			m_Ninja.m_ActivationTick = Server()->Tick(); // this should fix the end-of-ninja missprediction bug

		if ((m_Core.m_Frozen+1) % Server()->TickSpeed() == 0)
			GameServer()->CreateDamageInd(m_Pos, 0, (m_Core.m_Frozen+1) / Server()->TickSpeed());
	}
	else
	{
		if (m_ActiveWeapon == WEAPON_NINJA)
			TakeNinja();
	}
	if (m_Core.m_HookedPlayer != -1)
	{
		CCharacter* hookedChar = GameServer()->GetPlayerChar(m_Core.m_HookedPlayer);
		if (hookedChar)
			hookedChar->Interaction(GetPlayer()->GetCID());
	}

	DDWarTick();

	m_Armor = m_Core.m_Heat;

	// handle death-tiles and leaving gamelayer
	int Col = GameServer()->Collision()->GetCollisionAt(m_Pos.x, m_Pos.y);
	if(((Col&CCollision::COLFLAG_DEATH) && Col<=7) || GameLayerClipped(m_Pos)) //seriously, who could possibly care.
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
	else if (Col >= TILE_COLFRZ_FIRST && Col <= TILE_COLFRZ_LAST)
	{
		int ColIndex = Col - TILE_COLFRZ_FIRST;
		int Color = -1;
		switch(ColIndex)
		{//okay this is dirty but where are config arrays? :P
			case 0:Color = g_Config.m_SvColFrz0;break;
			case 1:Color = g_Config.m_SvColFrz1;break;
			case 2:Color = g_Config.m_SvColFrz2;break;
			case 3:Color = g_Config.m_SvColFrz3;break;
			case 4:Color = g_Config.m_SvColFrz4;break;
			case 5:Color = g_Config.m_SvColFrz5;break;
			case 6:Color = g_Config.m_SvColFrz6;break;
			case 7:Color = g_Config.m_SvColFrz7;break;
			case 8:Color = g_Config.m_SvColFrz8;break;
		}

		m_pPlayer->OverrideColors(Color);
	}
	else if (Col == TILE_BEGIN)
	{
		m_StartTick = Server()->Tick();
	}
	else if (Col == TILE_END)
	{
		if (m_StartTick != 0)
		{
			double seconds = (double)(Server()->Tick() - m_StartTick) / Server()->TickSpeed();
			char buf[500];
			/*if (g_Config.m_SvRaceFinishReward > 0 && m_pPlayer->GetAccount())
				str_format(buf, sizeof(buf), "%s finished in %.2f seconds and gained %.2f score", Server()->ClientName(m_pPlayer->GetCID()), seconds, (double)g_Config.m_SvRaceFinishReward);
			else */str_format(buf, sizeof(buf), "%s finished in %.2f seconds", Server()->ClientName(m_pPlayer->GetCID()), seconds);
			GameServer()->SendChat(-1, -2, buf);
			// ACCSERV HANDLE FINISH
			m_StartTick = 0;
		}
	}
	
	if (m_HammerScore > 0)
		m_HammerScore--;

	// handle Weapons
	HandleWeapons();

	if (m_DefEmoteReset >= 0 && m_DefEmoteReset <= Server()->Tick())
	{
		m_DefEmoteReset = -1;
		m_EmoteType = m_DefEmote = EMOTE_NORMAL;
		m_EmoteStop = -1;
	}

	// Previnput
	m_PrevInput = m_Input;
	return;
}

void CCharacter::TickDefered()
{
	// advance the dummy
	{
		CWorldCore TempWorld;
		m_ReckoningCore.Init(&TempWorld, GameServer()->Collision());
		m_ReckoningCore.Tick(false);
		m_ReckoningCore.Move();
		m_ReckoningCore.Quantize();
	}

	//lastsentcore
	vec2 StartPos = m_Core.m_Pos;
	vec2 StartVel = m_Core.m_Vel;
	bool StuckBefore = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));

	m_Core.Move();
	bool StuckAfterMove = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Core.Quantize();
	bool StuckAfterQuant = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Pos = m_Core.m_Pos;

	if(!StuckBefore && (StuckAfterMove || StuckAfterQuant))
	{
		// Hackish solution to get rid of strict-aliasing warning
		union
		{
			float f;
			unsigned u;
		}StartPosX, StartPosY, StartVelX, StartVelY;

		StartPosX.f = StartPos.x;
		StartPosY.f = StartPos.y;
		StartVelX.f = StartVel.x;
		StartVelY.f = StartVel.y;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "STUCK!!! %d %d %d %f %f %f %f %x %x %x %x",
			StuckBefore,
			StuckAfterMove,
			StuckAfterQuant,
			StartPos.x, StartPos.y,
			StartVel.x, StartVel.y,
			StartPosX.u, StartPosY.u,
			StartVelX.u, StartVelY.u);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	int Events = m_Core.m_TriggeredEvents;
	int64_t Mask = CmaskAllExceptOne(m_pPlayer->GetCID());

	if(Events&COREEVENT_GROUND_JUMP) GameServer()->CreateSound(m_Pos, SOUND_PLAYER_JUMP, Mask);

	if(Events&COREEVENT_HOOK_ATTACH_PLAYER) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_PLAYER, CmaskAll());
	if(Events&COREEVENT_HOOK_ATTACH_GROUND) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND, Mask);
	if(Events&COREEVENT_HOOK_HIT_NOHOOK) GameServer()->CreateSound(m_Pos, SOUND_HOOK_NOATTACH, Mask);


	if(m_pPlayer->GetTeam() == TEAM_SPECTATORS)
	{
		m_Pos.x = m_Input.m_TargetX;
		m_Pos.y = m_Input.m_TargetY;
	}
	
	IServer::CClientInfo CltInfo;
	Server()->GetClientInfo(m_pPlayer->GetCID(), &CltInfo);

	// update the m_SendCore if needed
	{
		CNetObj_Character Predicted;
		CNetObj_Character Current;
		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		m_ReckoningCore.Write(&Predicted);
		m_Core.Write(&Current);

		// only allow dead reackoning for a top of 3 seconds
		if(m_ReckoningTick+Server()->TickSpeed()*3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0
				|| (Server()->Tick()%2 == 0 && m_Core.m_Frozen > 0))
		{
			m_ReckoningTick = Server()->Tick();
			m_SendCore = m_Core;
			m_ReckoningCore = m_Core;
		}
	}
}

void CCharacter::TickPaused()
{
	++m_AttackTick;
	++m_DamageTakenTick;
	++m_Ninja.m_ActivationTick;
	++m_ReckoningTick;
	if(m_LastAction != -1)
		++m_LastAction;
	if(m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart > -1)
		++m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart;
	if(m_EmoteStop > -1)
		++m_EmoteStop;
}

bool CCharacter::IncreaseHealth(int Amount)
{
	if(m_Health >= 10)
		return false;
	m_Health = clamp(m_Health+Amount, 0, 10);
	return true;
}

bool CCharacter::IncreaseArmor(int Amount)
{
	if(m_Armor >= 10)
		return false;
	m_Armor = clamp(m_Armor+Amount, 0, 10);
	m_Core.m_Heat = m_Armor;
	return true;
}

void CCharacter::SendKillMsg(int Killer, int Weapon, int ModeSpecial)
{
	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Killer = Killer;
	Msg.m_Victim = m_pPlayer->GetCID();
	Msg.m_Weapon = Weapon;
	Msg.m_ModeSpecial = ModeSpecial;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CCharacter::Die(int Killer, int Weapon)
{
	// we got to wait 0.5 secs before respawning
	m_pPlayer->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()/2;
	int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, GameServer()->m_apPlayers[Killer], Weapon);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "kill killer='%d:%s' victim='%d:%s' weapon=%d special=%d",
		Killer, Server()->ClientName(Killer),
		m_pPlayer->GetCID(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// send the kill message
	//SendKillMsg(Killer, Weapon, ModeSpecial);
	BlockKill(true);

	// a nice sound
	GameServer()->CreateSound(m_Pos, SOUND_PLAYER_DIE);

	// this is for auto respawn after 3 secs
	m_pPlayer->m_DieTick = Server()->Tick();

	m_Alive = false;
	GameServer()->m_World.RemoveEntity(this);
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCID());
}

bool CCharacter::TakeDamage(vec2 Force, int Dmg, int From, int Weapon)
{
	m_Core.m_Vel += Force;
	
	if(!g_Config.m_SvDamage || (GameServer()->m_pController->IsFriendlyFire(m_pPlayer->GetCID(), From) && !g_Config.m_SvTeamdamage))
		return false;

	// m_pPlayer only inflicts half damage on self
	if(From == m_pPlayer->GetCID())
		Dmg = max(0, Dmg/2);

	m_DamageTaken++;

	// create healthmod indicator
	if(Server()->Tick() < m_DamageTakenTick+25)
	{
		// make sure that the damage indicators doesn't group together
		GameServer()->CreateDamageInd(m_Pos, m_DamageTaken*0.25f, Dmg);
	}
	else
	{
		m_DamageTaken = 0;
		GameServer()->CreateDamageInd(m_Pos, 0, Dmg);
	}

	if(Dmg)
	{
		/*if(m_Armor)
		{
			if(Dmg > 1)
			{
				m_Health--;
				Dmg--;
			}

			if(Dmg > m_Armor)
			{
				Dmg -= m_Armor;
				m_Armor = 0;
			}
			else
			{
				m_Armor -= Dmg;
				Dmg = 0;
			}
		}*/

		m_Health -= Dmg;
	}

	m_DamageTakenTick = Server()->Tick();
	

	// do damage Hit sound
	if(From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
	{
		int64_t Mask = CmaskOne(From);
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS && GameServer()->m_apPlayers[i]->m_SpectatorID == From)
				Mask |= CmaskOne(i);
		}
		GameServer()->CreateSound(GameServer()->m_apPlayers[From]->m_ViewPos, SOUND_HIT, Mask);
	}

	// check for death
	if(m_Health <= 0)
	{
		Die(From, Weapon);

		// set attacker's face to happy (taunt!)
		if (From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
		{
			CCharacter *pChr = GameServer()->m_apPlayers[From]->GetCharacter();
			if (pChr)
			{
				pChr->m_EmoteType = EMOTE_HAPPY;
				pChr->m_EmoteStop = Server()->Tick() + Server()->TickSpeed();
			}
		}

		return false;
	}

	if (Dmg > 2)
		GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_LONG);
	else
		GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_SHORT);

	m_EmoteType = EMOTE_PAIN;
	m_EmoteStop = Server()->Tick() + 500 * Server()->TickSpeed() / 1000;

	return true;
}

void CCharacter::Snap(int SnappingClient)
{
	CNetObj_Character Measure; // used only for measuring the offset between vanilla and extended core
	int id = m_pPlayer->GetCID();

	if (!Server()->Translate(id, SnappingClient))
		return;

	if(NetworkClipped(SnappingClient))
		return;
	
	IServer::CClientInfo CltInfo;
	Server()->GetClientInfo(SnappingClient, &CltInfo);
	CltInfo.m_CustClt = 0;

	// set emote
	if (m_EmoteStop < Server()->Tick())
	{
		m_EmoteType = m_DefEmote;
		m_EmoteStop = -1;
	}

	CNetObj_Character *pCharacter;

	// measure distance between first extended and first vanilla field
	size_t Offset = (char*)(&Measure.m_Tick) - (char*)(&Measure.m_Frz);

	// vanilla size for vanilla clients, extended for custom client
	size_t Sz = sizeof (CNetObj_Character) - (CltInfo.m_CustClt?0:Offset);

	// create a snap item of the size the client expects
	pCharacter = static_cast<CNetObj_Character *>(Server()->SnapNewItem(NETOBJTYPE_CHARACTER, id, Sz));

	if(!pCharacter)
		return;
	
	// for vanilla clients, make pCharacter point before the start our snap item start, so that the vanilla core
	// aligns with the snap item. we may not access the extended fields then, since they are out of bounds (to the left)
	if (!CltInfo.m_CustClt)
		pCharacter = (CNetObj_Character*)(((char*)pCharacter)-Offset); // moar cookies.

	if(!m_ReckoningTick || GameServer()->m_World.m_Paused)
	{
		pCharacter->m_Tick = 0;
		m_Core.Write(pCharacter, !CltInfo.m_CustClt);
	}
	else
	{
		pCharacter->m_Tick = m_ReckoningTick;
		m_SendCore.Write(pCharacter, !CltInfo.m_CustClt);
	}

	if (pCharacter->m_HookedPlayer != -1)
	{
		if (!Server()->Translate(pCharacter->m_HookedPlayer, SnappingClient))
			pCharacter->m_HookedPlayer = -1;
	}
	pCharacter->m_Emote = m_EmoteType;

	pCharacter->m_AmmoCount = 0;
	pCharacter->m_Health = 0;
	pCharacter->m_Armor = 0;

	pCharacter->m_Weapon = m_ActiveWeapon == -1 ? (g_Config.m_SvNowepsKnife ? NUM_WEAPONS : WEAPON_GUN) : m_ActiveWeapon; // dangerous
	pCharacter->m_AttackTick = m_AttackTick;

	pCharacter->m_Direction = m_Input.m_Direction;

	if(m_pPlayer->GetCID() == SnappingClient || SnappingClient == -1 ||
		(!g_Config.m_SvStrictSpectateMode && m_pPlayer->GetCID() == GameServer()->m_apPlayers[SnappingClient]->m_SpectatorID))
	{
		pCharacter->m_Health = m_Health;
		pCharacter->m_Armor = m_Armor;
		if(m_ActiveWeapon != -1 && m_aWeapons[m_ActiveWeapon].m_Ammo > 0)
			pCharacter->m_AmmoCount = m_aWeapons[m_ActiveWeapon].m_Ammo;
	}

	if(pCharacter->m_Emote == EMOTE_NORMAL)
	{
		if(250 - ((Server()->Tick() - m_LastAction)%(250)) < 5)
			pCharacter->m_Emote = EMOTE_BLINK;
		if(m_Core.m_Frozen || m_State == BS_BLOCKED)
			pCharacter->m_Emote = EMOTE_BLINK;
		if ((m_ChattingSince!=0 && Ago(m_ChattingSince, g_Config.m_SvChatblockTime) && m_State == BS_FREE) || m_Chatblocked)
			pCharacter->m_Emote = EMOTE_SURPRISE;
	}

	pCharacter->m_PlayerFlags = GetPlayer()->m_PlayerFlags;
}

void CCharacter::DDWarTick()
{
	if (HasFlag() == 0 && g_Config.m_SvFlag0HeatRegen !=0 && Server()->Tick()%(Server()->TickSpeed()*g_Config.m_SvFlag0HeatRegen)==0 && m_Armor < g_Config.m_SvFlag0HeatCap)
		IncreaseArmor(1);

	if (HasFlag() == 1 && g_Config.m_SvFlag1HeatRegen !=0 && Server()->Tick()%(Server()->TickSpeed()*g_Config.m_SvFlag1HeatRegen)==0 && m_Armor < g_Config.m_SvFlag1HeatCap)
		IncreaseArmor(1);

	if (Ago(m_CKPunishTick, 10000))
	{
		m_CKPunishTick = 0;
		m_CKPunish = 0;
	}
	if (m_pPlayer->m_PlayerFlags&PLAYERFLAG_CHATTING)
	{
		if (m_ChattingSince == 0 && m_State == BS_FREE)
			m_ChattingSince = Server()->Tick();
	}
	else
	{
		if (m_State != BS_FROZEN)
			m_ChattingSince = 0;
		if (m_State == BS_INTERACTED)
			m_Chatblocked = false;
	}
	if (m_Core.m_HookedPlayer != -1)
	{
		CCharacter* hooked = GameServer()->GetPlayerChar(m_Core.m_HookedPlayer);
		if (hooked)
			hooked->Interaction(GetPlayer()->GetCID());
	}
	if (m_Core.m_Frozen)
		m_LastFrozen = Server()->Tick();
	if (m_State == BS_FREE)
	{
		if (m_Killer != -1) NewState(BS_INTERACTED);
		else if (m_Core.m_Frozen) NewState(BS_SELFFREEZED);
	} else if (m_State == BS_SELFFREEZED)
	{
		if (Ago(m_LastStateChange, g_Config.m_SvSelfBlocked)) NewState(BS_BLOCKED);
		else if (!m_Core.m_Frozen) NewState(BS_FREE);
	} else if (m_State == BS_INTERACTED)
	{
		if (m_Core.m_Frozen) NewState(BS_FROZEN);
		else if (Ago(m_KillerTick, g_Config.m_SvIntFree)) NewState(BS_FREE);
	} else if (m_State == BS_FROZEN)
	{
		if (Ago(m_LastStateChange, g_Config.m_SvFrozenBlocked)) NewState(BS_BLOCKED);
		else if (Ago(m_LastFrozen, g_Config.m_SvFrozenInt)) NewState(BS_INTERACTED);
	} else if (m_State == BS_BLOCKED)
	{
		if (Ago(m_LastFrozen, g_Config.m_SvBlockedFree) && !Ago(m_pPlayer->m_LastActionTick, 500)) NewState(BS_FREE);
	}
}

void CCharacter::Interaction(int with)
{
	if (with < 0 || with >= MAX_CLIENTS) return;
	if (!GameServer()->m_apPlayers[with]) return;
	with = GameServer()->m_apPlayers[with]->GetCUID();
	if (with == GetPlayer()->GetCUID()) return;
	if (m_State == BS_FREE || m_State == BS_INTERACTED || (m_Killer == with && m_State == BS_FROZEN))
		SetKiller(with);
	if (m_State == BS_BLOCKED && !Ago(m_LastFrozen, 3))
		SetHelper(with);
	CGameControllerMOD* mod = (CGameControllerMOD*)GameServer()->m_pController;
	if (m_State == BS_BLOCKED)
	{
		CCharacter* withChar = GameServer()->GetPlayerByUID(with)->GetCharacter();
		if (withChar)
			for (int i=0;i<2;i++)
				if (mod->GetFlagCarrier(i) == this)
					mod->SetFlagCarrier(i, withChar);
	}
}

void CCharacter::SetKiller(int killerUID)
{
	m_Killer = killerUID;
	m_KillerTick = Server()->Tick();
}

void CCharacter::SetHelper(int helperUID)
{
	m_Helper = helperUID;
	m_HelperTick = Server()->Tick();
}

bool CCharacter::Ago(int tick, int millis)
{
	return (tick < Server()->Tick()-Server()->TickSpeed()*millis/1000);
}

void CCharacter::NewState(int state)
{
	if (state == BS_FREE)
	{
		BlockHelp();
		SetKiller(-1);
		SetHelper(-1);
	}
	if (state == BS_SELFFREEZED)
	{
		m_ChattingSince = 0;
	}
	if (state == BS_BLOCKED)
	{
		BlockKill(false);
	}
	if (state == BS_INTERACTED)
	{
		m_Chatblocked = (m_ChattingSince!=0 && Ago(m_ChattingSince, g_Config.m_SvChatblockTime));
	}
	else if (state != BS_FROZEN)
	{
		m_Chatblocked = false;
	}
	m_State = state;
	m_LastStateChange = Server()->Tick();
}

void CCharacter::BlockHelp()
{
	if (!Ago(m_HelperTick, 14000) && m_Helper != -1)
	{
		CPlayer* helper = GameServer()->GetPlayerByUID(m_Helper);
		if (helper)
		{
			CNetMsg_Sv_KillMsg Msg;
			Msg.m_Killer = helper->GetCID();
			Msg.m_Victim = GetPlayer()->GetCID();
			Msg.m_Weapon = WEAPON_RIFLE;
			Msg.m_ModeSpecial = 0;
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
			char buf[300];
			str_format(buf, sizeof(buf), "player %d has /helped %d", m_Helper, GetPlayer()->GetCUID());
			GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "hook", buf);
		}
	}
}

void CCharacter::BlockKill(bool dead)
{
	m_ChattingSince = 0;
	if (m_Helper == m_Killer)
		SetHelper(-1);
	CPlayer* killer = GameServer()->GetPlayerByUID(m_Killer);
	int killerID = GetPlayer()->GetCID();
	int killerUID = GetPlayer()->GetCUID();
	if (killer)
	{
		killerID = killer->GetCID();
		killerUID = killer->GetCUID();
	}
	if (!dead && killerID == GetPlayer()->GetCID())
		return;

	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Killer = killerID;
	Msg.m_Victim = GetPlayer()->GetCID();
	Msg.m_Weapon = dead ? WEAPON_NINJA : WEAPON_HAMMER;
	Msg.m_ModeSpecial = 0;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);

	m_Killer = -1;

	if (!killer)
		return;

	if (m_Chatblocked)
	{
		char buf[300];
		str_format(buf, sizeof(buf), "%s chatkilled %s", Server()->ClientName(killerID), Server()->ClientName(GetPlayer()->GetCID()));
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "ddwar", buf);

		vec2 pos;
		pos.x = 0;
		pos.y = -100;
		vec2 vel;
		vel.x = 0;
		vel.y = 0;
		CLoltext::Create(&GameServer()->m_World, this, pos, vel, 200, "CHATKILL", 1, 0);
		if (g_Config.m_SvChatblockPunish != 0)
		{
			m_CKPunish = killerUID;
			m_CKPunishTick = Server()->Tick() + Server()->TickSpeed()*10;
			str_format(GetPlayer()->m_PersonalBroadcast, sizeof(GetPlayer()->m_PersonalBroadcast), "%s chatkilled you! type /p to punish", Server()->ClientName(killer->GetCID()));
			GetPlayer()->m_PersonalBroadcastTick = Server()->Tick() + Server()->TickSpeed()*10;
			GameServer()->SendBroadcast(GetPlayer()->m_PersonalBroadcast, GetPlayer()->GetCID());
		}
	}
	else
	{
		char buf[300];
		str_format(buf, sizeof(buf), "player %d has /killed %d", killer->GetCID(), GetPlayer()->GetCUID());
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "hook", buf);

		if (killer->GetCharacter())
			for (int i=0;i<2;i++)
				if (((CGameControllerMOD*)GameServer()->m_pController)->GetFlagCarrier(i) == this)
					((CGameControllerMOD*)GameServer()->m_pController)->SetFlagCarrier(i, killer->GetCharacter());
	}
}

void CCharacter::Freeze(int ticks)
{
	if (m_Core.m_Frozen < ticks)
		m_Core.m_Frozen = ticks;
}

vec2 CCharacter::GetPos()
{
	return m_Core.m_Pos;
}

void CCharacter::SetPos(vec2 pos)
{
	m_Core.m_Pos = pos;
}

int CCharacter::HasFlag()
{
	CGameControllerMOD* mod = (CGameControllerMOD*)GameServer()->m_pController;
	for (int i=0;i<2;i++)
		if (mod->GetFlagCarrier(i) == this)
			return i;
	return -1;
}
