
#include "resolver.h"
#include "../../../options.hpp"
#include "../autowall/ragebot-autowall.h"
#include "../ragebot.h"
#include <algorithm>
#include "../../../valve_sdk/interfaces/IGameEventmanager.hpp"
#include "../../event-logger/event-logger.h"

static float ResolvedYaw[65];
#ifndef max
#define max(a,b)            (((a) > (b)) ? (a) : (b))
#endif

#ifndef min
#define min(a,b)            (((a) < (b)) ? (a) : (b))
#endif

float eyeAngleDiff (float destAngle, float srcAngle) {
	float delta = fmodf(destAngle - srcAngle, 360.0f);

	if (destAngle > srcAngle)
	{
		if (delta >= 180)
			delta -= 360;
	}
	else
	{
		if (delta <= -180)
			delta += 360;
	}

	return delta;
}

void Resolver::PreverseSafePoint (C_BasePlayer * pPlayer , int iSafeSide, float flTime ) {

	// Selfcoded by @5N4K3 & @D1spemel
	// Own u and all !

	// Store var's
	const auto flPoses       = pPlayer->m_flPoseParameter();
	const auto angEyeAngles  = pPlayer->m_angEyeAngles();
	const auto vecVelocity   = pPlayer->m_vecVelocity();
	const auto vecOrigin     = pPlayer->m_vecOrigin();
	const auto flDuckAmount  = pPlayer->m_flDuckAmount();
	const auto flSimtime     = pPlayer->m_flSimulationTime();
	const auto fFlags        = pPlayer->m_fFlags();

	pPlayer->ForceBoneRebuilid   ();
	pPlayer->InvalidateBoneCache ();

	pPlayer->GetEffect() |= 8;

	if   (iSafeSide == -1) {

		pPlayer->GetPlayerAnimState()->m_flGoalFeetYaw = Math::NormalizeYaw(pPlayer->m_angEyeAngles().yaw - 60);

		pPlayer->SetupBones (pLeftMatrix, 128, BONE_USED_BY_ANYTHING, flTime);
		// Left matrix

	}
	else if (iSafeSide == 0) {
		pPlayer->GetPlayerAnimState()->m_flGoalFeetYaw = Math::NormalizeYaw(pPlayer->m_angEyeAngles().yaw);
		pPlayer->SetupBones (pMiddleMatrix, 128, BONE_USED_BY_ANYTHING, flTime);
		// Center matrix

	}
	else if (iSafeSide == 1) {
		pPlayer->GetPlayerAnimState()->m_flGoalFeetYaw = Math::NormalizeYaw(pPlayer->m_angEyeAngles().yaw + 60);
		pPlayer->SetupBones (pRightMatrix, 128, BONE_USED_BY_ANYTHING, flTime);
		// Right matrix

	}
	LagCompensation::Get().UpdateAnimationsData(pPlayer);

	// Restore var's.
	pPlayer->m_vecVelocity() = vecVelocity;
	pPlayer->m_vecOrigin() = vecOrigin;
	pPlayer->m_flDuckAmount() = flDuckAmount;
	pPlayer->m_flSimulationTime() = flSimtime;
	pPlayer->m_angEyeAngles() = angEyeAngles;
	pPlayer->m_fFlags() = fFlags;
	pPlayer->m_vecAbsVelocity() = vecVelocity;

	pPlayer->GetEffect() &= ~8;

}


void Resolver::StoreStatusPlayer(C_BasePlayer* pPlayer, int resolve_info, int side, bool backward) {
	if (side == 0) {
		ResolveRecord[pPlayer->EntIndex()].info = "Middle";
	}
		else
	ResolveRecord[pPlayer->EntIndex()].info = side == -1 ? "Max Left" :  "Max Right";

	ResolveRecord[pPlayer->EntIndex()].info += backward ? "Backward" : "Forward";
}

float GetAngle(C_BasePlayer* player) {
	return Math::NormalizeYaw(player->m_angEyeAngles().yaw);
}
float GetBackwardYaw(C_BasePlayer* player) {
	return Math::CalculateAngle(g_LocalPlayer->m_angAbsOrigin(), player->m_angAbsOrigin()).y;
}
float GetForwardYaw(C_BasePlayer* player) {
	return Math::NormalizeYaw(GetBackwardYaw(player) - 180.f);
}
float get_max_desync_delta(C_BasePlayer* ent) {

	auto animstate = ent->GetPlayerAnimState();
	float duckamount = animstate->m_fDuckAmount;// + 0xA4;

	float speedfraction = max(0, min(animstate->m_flFeetSpeedForwardsOrSideWays, 1));
	float speedfactor = max(0, min(animstate->m_flFeetSpeedUnknownForwardOrSideways, 1));

	float unk1 = ((*reinterpret_cast<float*> ((uintptr_t)animstate + 0x11C) * -0.30000001) - 0.19999999)* speedfraction;
	float unk2 = unk1 + 1.f;

	if (duckamount > 0.0)
		unk2 += ((duckamount * speedfactor) * (0.5f - unk2));

	return (*(float*)((uintptr_t)animstate + 0x334)) * unk2;
}
bool TraceLine(Vector& start, Vector& end, unsigned int mask, C_BasePlayer* ignore, trace_t* trace, C_BasePlayer* pPlayer)
{
	Ray_t ray;
	ray.Init(start, end);

	CTraceFilter filter;
	filter.pSkip = ignore;

	g_EngineTrace->TraceRay(ray, mask, &filter, trace);

	if (!filter.ShouldHitEntity(pPlayer, MASK_SHOT))
		return false;
	else
		return true;

}
bool IsNearEqual(float v1, float v2, float Tolerance)
{
	return std::abs(v1 - v2) <= std::abs(Tolerance);
};
void Resolver::DetectFakeSide (C_BasePlayer * pPlayer) {

	if (!pPlayer) return;
	auto			Index			= pPlayer->EntIndex();
	auto			&rRecord		= ResolveRecord[Index];
	float			flYaw			= pPlayer->m_angEyeAngles().yaw;
	int				missedshots		= 0;
	int				last_side;
	static float	LastAngle[64];
	static int		LastBrute[64];
	static bool		Switch[64];
	static float	LastUpdateTime[64];
	int				i = pPlayer->GetIndex();
	float			CurrentAngle = pPlayer->m_angEyeAngles().yaw;

	int		   resolve_value	= get_max_desync_delta(pPlayer);
	const auto Choked			= max(0, TIME_TO_TICKS(pPlayer->m_flSimulationTime() - pPlayer->m_flOldSimulationTime()) - 1);
	bool       backward			= !(fabsf(Math::NormalizeYaw(GetAngle(pPlayer) - GetForwardYaw(pPlayer))) < 90.f);
	auto	   balance_adjust	= (pPlayer->GetAnimOverlays()[3].m_flWeight > 0.01f && pPlayer->GetSequenceActivity(pPlayer->GetAnimOverlays()[3].m_nSequence) == 979 && pPlayer->GetAnimOverlays()[3].m_flCycle < 1.f);


	StoreResolveDelta(pPlayer, &rRecord);

	if ( pPlayer->m_fFlags() & FL_ONGROUND ) {

		Vector src3D, dst3D, forward, right, up, src, dst;
		float back_two, right_two, left_two;
		trace_t tr;
		Ray_t ray, ray2, ray3, ray4, ray5;
		CTraceFilter filter;

		Math::AngleVectors(QAngle(0, GetBackwardYaw(pPlayer), 0), forward, right, up);

		filter.pSkip = pPlayer;
		src3D = pPlayer->GetEyePos();
		dst3D = src3D + (forward * 384);

		ray.Init(src3D, dst3D);
		g_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
		back_two = (tr.endpos - tr.startpos).Length();

		ray2.Init(src3D + right * 35, dst3D + right * 35);
		g_EngineTrace->TraceRay(ray2, MASK_SHOT, &filter, &tr);
		right_two = (tr.endpos - tr.startpos).Length();

		ray3.Init(src3D - right * 35, dst3D - right * 35);
		g_EngineTrace->TraceRay(ray3, MASK_SHOT, &filter, &tr);
		left_two = (tr.endpos - tr.startpos).Length();

		if (left_two > right_two) {
			rRecord.antifreestand_side = -1;
			//Body should be right
		}
		else if (right_two > left_two) {
			rRecord.antifreestand_side = 1;
		}
		else
			rRecord.antifreestand_side = 0;

		if ( rRecord.antifreestand_side != 0 ) {
			if (pPlayer->m_vecVelocity().Length2D() < 0.15f) {
				auto Delta = eyeAngleDiff(pPlayer->m_angEyeAngles().yaw, pPlayer->GetPlayerAnimState()->m_flGoalFeetYaw);

				if (pPlayer->GetAnimOverlays()[3].m_flWeight == 0.0f && pPlayer->GetAnimOverlays()[3].m_flCycle == 0.0f) {
					rRecord.iResolvingWay = Math::Clamp((2 * (Delta <= 0.f) - 1), -1, 1);
					rRecord.bWasUpdated = true;
				}
			}
			else {

				float Rate = abs(pPlayer->GetAnimOverlays()[6].m_flPlaybackRate - rRecord.ResolverLayers[0][6].m_flPlaybackRate);
				float Rate2 = abs(pPlayer->GetAnimOverlays()[6].m_flPlaybackRate - rRecord.ResolverLayers[1][6].m_flPlaybackRate);
				float Rate3 = abs(pPlayer->GetAnimOverlays()[6].m_flPlaybackRate - rRecord.ResolverLayers[2][6].m_flPlaybackRate);

				
				if (Rate < Rate3 || Rate2 <= Rate3 || (int)(float)(Rate3 * 1000.0f)) {
					if (Rate >= Rate2 && Rate3 > Rate2 && !(int)(float)(Rate2 * 1000.0f)) {

						if (rRecord.antifreestand_side == 1) {
							rRecord.iResolvingWay = 1;
							rRecord.bWasUpdated = true;
						}
						else {
							rRecord.iResolvingWay = -1;
						}

					}
				}
				else {
					if (rRecord.antifreestand_side == -1) {
						rRecord.iResolvingWay = -1;
						rRecord.bWasUpdated = true;
					}
					else {
						rRecord.iResolvingWay = 1;
						rRecord.bWasUpdated = true;
					}
				}
			}
		}

		if (!IsNearEqual(CurrentAngle, LastAngle[i], 50.f)) {
			Switch[i] = !Switch[i];
			LastAngle[i] = CurrentAngle;
			rRecord.JitterSide = Switch[i] ? 1 : -1;
			LastBrute[i] = rRecord.JitterSide;
			LastUpdateTime[i] = g_GlobalVars->curtime;
			rRecord.IsJitter = true;
		}
		else {
			if (fabsf(LastUpdateTime[i] - g_GlobalVars->curtime >= TICKS_TO_TIME(17))
				|| pPlayer->m_flSimulationTime() != pPlayer->m_flOldSimulationTime()) {
				LastAngle[i] = CurrentAngle;
			}
			rRecord.JitterSide = LastBrute[i];
		}
		rRecord.IsJitter = false;

		/* Update records :  */ {
			if (balance_adjust) /* LBY desync mode detection */ {
				rRecord.LastBalancedDesync = g_GlobalVars->realtime;
			}
			rRecord.lby_delta = Math::NormalizeYaw(pPlayer->m_flLowerBodyYawTarget() - pPlayer->m_angEyeAngles().yaw);
			rRecord.desyncmode_balance = (fabs(g_GlobalVars->realtime - rRecord.LastBalancedDesync) < 0.6f && fabs(rRecord.lby_delta) > 0.0001f);
			if (pPlayer->m_angEyeAngles().pitch > 84 || pPlayer->m_angEyeAngles().pitch == -89 )
				rRecord.LastPitchDown = g_GlobalVars->realtime;
			if (!pPlayer->IsAlive()) {
				ResolveRecord[Index].lastalive = g_GlobalVars->realtime;
			}
			/* fakelags */ {
				auto ticks = (TIME_TO_TICKS(pPlayer->m_flSimulationTime() - pPlayer->m_flOldSimulationTime()) > 1);
				rRecord.has_fakelags = (rRecord.fl > 2);
				if (ticks == 0 && rRecord.last_ticks > 0) {
					rRecord.fl = rRecord.last_ticks - 1;
				}
				else {
					rRecord.last_ticks = ticks;
					rRecord.fl = ticks;
				}
			}
			if (pPlayer->GetPlayerInfo().fakeplayer)
				rRecord.has_fake = true;
			else
			rRecord.has_fake = (fabs(g_GlobalVars->realtime - rRecord.LastPitchDown) < 0.8f) ? true : false;
		}

		if ( (!GetAsyncKeyState(g_Options.ragebot_force_safepoint)) && g_Options.ragebot_resolver) {

			if (rRecord.iResolvingWay < 0) {

				if (rRecord.iMissedShots != 0) {
					//bruteforce player angle accordingly
					switch (rRecord.iMissedShots % 2) {
					case 1: {
						if (backward) {
							flYaw += resolve_value;
							StoreStatusPlayer(pPlayer, resolve_value, 1, backward);

						}
						else {
							flYaw -= resolve_value;
						    StoreStatusPlayer(pPlayer, resolve_value, -1, backward);
						}
					}
					break;

					case 0: {
						if (backward) {
							flYaw -= resolve_value;
							StoreStatusPlayer(pPlayer, resolve_value, -1, backward);
						}
						else {
							flYaw += resolve_value;
							StoreStatusPlayer(pPlayer, resolve_value, 1, backward);
						}
					}
					break;


					}
				}
				else {
					if (backward) {
						flYaw -= resolve_value;
						StoreStatusPlayer(pPlayer, resolve_value, -1, backward);
					}
					else {
						flYaw += resolve_value;
						StoreStatusPlayer(pPlayer, resolve_value, 1, backward);
					}
				}
			}
			else if (rRecord.iResolvingWay > 0) {

				if (rRecord.iMissedShots != 0) {
					//bruteforce player angle accordingly
					switch (rRecord.iMissedShots % 2) {
					case 1: {
						if (backward) {
							flYaw -= resolve_value;
							StoreStatusPlayer(pPlayer, resolve_value, -1, backward);

						}
						else {
							flYaw += resolve_value;
							StoreStatusPlayer(pPlayer, resolve_value, 1, backward);
						}
					}
					break;

					case 0: {
						if (backward) {
							flYaw += resolve_value;
							StoreStatusPlayer(pPlayer, resolve_value, 1, backward);
						}
						else {
							flYaw -= resolve_value;
							StoreStatusPlayer(pPlayer, resolve_value, -1, backward);
						}
					}
					break;


					}
				}
				else {
					if (backward) {
						flYaw += resolve_value;
						StoreStatusPlayer(pPlayer, resolve_value, 1, backward);
					}
					else {
						flYaw -= resolve_value;
						StoreStatusPlayer(pPlayer, resolve_value, -1, backward);
					}
				}
			}
			
			Math::NormalizeYaw(flYaw						);
			Math::NormalizeYaw(pPlayer->m_angEyeAngles().yaw);

			int New_iYaw = rRecord.IsJitter ? pPlayer->m_angEyeAngles().yaw + ((flYaw - pPlayer->m_angEyeAngles().yaw ) * (-1)) : flYaw;

			if (g_Options.ragebot_resolver)
				pPlayer->GetPlayerAnimState()->m_flGoalFeetYaw = New_iYaw;
			else
				pPlayer->GetPlayerAnimState()->m_flGoalFeetYaw = pPlayer->m_angEyeAngles().yaw;

		}
		
	}
}




void Resolver::StoreResolveDelta(C_BasePlayer * pPlayer,ResolveInfo * cData) {

	const auto flPoses        = pPlayer->m_flPoseParameter();
	const auto angEyeAngles   = pPlayer->m_angEyeAngles();
	const auto vecVelocity    = pPlayer->m_vecVelocity();
	const auto vecOrigin      = pPlayer->m_vecOrigin();
	const auto flDuckAmount   = pPlayer->m_flDuckAmount();
	const auto flSimtime      = pPlayer->m_flSimulationTime();
	const auto fFlags         = pPlayer->m_fFlags();


	std::memcpy(cData->ResolverLayers[0], pPlayer->GetAnimOverlays(), (sizeof(AnimationLayer) * pPlayer->GetNumAnimOverlays()));

	pPlayer->ForceBoneRebuilid();
	pPlayer->m_vecVelocity()      = vecVelocity;
	pPlayer->m_vecOrigin()        = vecOrigin;
	pPlayer->m_flDuckAmount()     = flDuckAmount;
	pPlayer->m_flSimulationTime() = flSimtime;
	pPlayer->m_angEyeAngles()     = angEyeAngles;
	pPlayer->m_fFlags()           = fFlags;
	pPlayer->m_vecAbsVelocity()   = vecVelocity;


	std::memcpy(cData->ResolverLayers[1], pPlayer->GetAnimOverlays(), (sizeof(AnimationLayer) * pPlayer->GetNumAnimOverlays()));

	pPlayer->ForceBoneRebuilid();
	pPlayer->m_vecVelocity() = vecVelocity;
	pPlayer->m_vecOrigin() = vecOrigin;
	pPlayer->m_flDuckAmount() = flDuckAmount;
	pPlayer->m_flSimulationTime() = flSimtime;
	pPlayer->m_angEyeAngles() = angEyeAngles;
	pPlayer->m_fFlags() = fFlags;
	pPlayer->m_vecAbsVelocity() = vecVelocity;

	std::memcpy(cData->ResolverLayers[2], pPlayer->GetAnimOverlays(), (sizeof(AnimationLayer) * pPlayer->GetNumAnimOverlays()));


	pPlayer->ForceBoneRebuilid();
	pPlayer->m_vecVelocity()      = vecVelocity;
	pPlayer->m_vecOrigin()        = vecOrigin;
	pPlayer->m_flDuckAmount()     = flDuckAmount;
	pPlayer->m_flSimulationTime() = flSimtime;
	pPlayer->m_angEyeAngles()     = angEyeAngles;
	pPlayer->m_fFlags()           = fFlags;
	pPlayer->m_vecAbsVelocity()   = vecVelocity;

}
