#include "precompiled.h"

cvar_t cv_resemiclip_version = { "resemiclip_version", Plugin_info.version, FCVAR_SERVER | FCVAR_EXTDLL, 0, nullptr };
CGamePlayer g_Players[MAX_CLIENTS];
CConfig g_Config;
CGameData g_GameData;
CBaseEntity *g_pVirtualGrenade;
bool bInitialized = false;
bool bActivated = false;

struct {
	const char *classname;
	CBaseEntity *&ptr;
}

g_RegVirtualTable[] = {
	{ "grenade", g_pVirtualGrenade }
};

int OnMetaAttach()
{
	Load_Config();

	g_engfuncs.pfnCvar_RegisterVariable(&cv_resemiclip_version);

	REG_SVR_COMMAND("semiclip_option", SVR_SemiclipOption);

	g_RehldsHookchains->SV_CreatePacketEntities()->registerHook(&SV_CreatePacketEntities, HC_PRIORITY_HIGH);

	g_ReGameHookchains->CBasePlayer_Spawn()->registerHook(&CBasePlayer_Spawn, HC_PRIORITY_DEFAULT + 1);
	g_ReGameHookchains->RadiusFlash_TraceLine()->registerHook(&RadiusFlash_TraceLine, HC_PRIORITY_HIGH);

	if (g_Config.GetTime()) {
		g_ReGameHookchains->CSGameRules_OnRoundFreezeEnd()->registerHook(&CSGameRules_OnRoundFreezeEnd, HC_PRIORITY_DEFAULT + 1);
	}

	if (g_Config.GetPenetFire()) {
		g_ReGameHookchains->CBasePlayer_TraceAttack()->registerHook(&CBasePlayer_TraceAttack, HC_PRIORITY_DEFAULT + 1);
	}

	return TRUE;
}

void OnMetaDetach()
{
	g_pFunctionTable->pfnPM_Move = nullptr;
	g_pNewFunctionTable->pfnShouldCollide = nullptr;

	g_RehldsHookchains->SV_CreatePacketEntities()->unregisterHook(&SV_CreatePacketEntities);

	g_ReGameHookchains->InstallGameRules()->unregisterHook(&InstallGameRules);
	g_ReGameHookchains->CBasePlayer_Spawn()->unregisterHook(&CBasePlayer_Spawn);
	g_ReGameHookchains->RadiusFlash_TraceLine()->unregisterHook(&RadiusFlash_TraceLine);

	if (g_Config.GetTime()) {
		g_ReGameHookchains->CSGameRules_OnRoundFreezeEnd()->unregisterHook(&CSGameRules_OnRoundFreezeEnd);
	}

	if (g_Config.GetPenetFire()) {
		g_ReGameHookchains->CBasePlayer_TraceAttack()->unregisterHook(&CBasePlayer_TraceAttack);
	}

	bActivated = false;
	bInitialized = false;
}

void CGameData::Init()
{
	SetStartEdict(INDEXENT(0));
}

inline void ClearAllClients()
{
	for (int i = 0; i < g_GameData.GetMaxClients(); i++)
	{
		CGamePlayer *pPlayer = PLAYER_FOR_NUM(i);

		if (!pPlayer) {
			continue;
		}

		pPlayer->Player_Clear();
	}
}

void ServerActivate_Post(edict_t *pEdictList, int edictCount, int clientMax)
{
	bActivated = true;

	g_GameData.SetMaxClients(clientMax);
	g_GameData.SetMaxClientEdict(pEdictList + clientMax);
	g_GameData.SetStartEnt(pEdictList + 1);

	for (int i = 0; i < clientMax; i++)
	{
		CGamePlayer *pPlayer = PLAYER_FOR_NUM(i);

		if (!pPlayer) {
			continue;
		}

		pPlayer->Init(pEdictList, i);
	}

	for (auto& ent : g_RegVirtualTable)
	{
		edict_t *pEntity = CREATE_ENTITY();

		CALL_GAME_ENTITY(PLID, ent.classname, VARS(pEntity));

		if (pEntity->pvPrivateData) {
			ent.ptr = *(CBaseEntity **)pEntity->pvPrivateData;
		}

		REMOVE_ENTITY(pEntity);
	}

	Load_Config_Maps();

	SET_META_RESULT(MRES_IGNORED);
}

void ServerDeactivate_Post()
{
	if (bInitialized)
	{
		if (bActivated)
		{
			ClearAllClients();
		}

		bInitialized = false;
		bActivated = false;
	}

	SET_META_RESULT(MRES_IGNORED);
}

int DispatchSpawn(edict_t *pEntity)
{
	if (!bInitialized)
	{
		g_GameData.Init();

		bActivated = false;
		bInitialized = true;
	}

	RETURN_META_VALUE(MRES_IGNORED, META_RESULT_ORIG_RET(int));
}

void CGamePlayer::Init(edict_t *pEdictList, int index)
{
	this->m_iIndex = index + 1;
}

void CGamePlayer::Player_Clear()
{
	m_bDont = false;

	for (int i = 0; i <= g_GameData.GetMaxClients(); i++)
	{
		m_fDiff[i] = 0.0f;
		m_bCrouch[i] = false;
		m_bSolid[i] = false;
	}
}

void ClientDisconnect(edict_t *pEdict)
{
	if (bActivated)
	{
		CGamePlayer *pPlayer = PLAYER_FOR_EDICT(pEdict);

		if (pPlayer != nullptr)
		{
			pPlayer->Player_Clear();
		}
	}

	SET_META_RESULT(MRES_IGNORED);
}

static inline bool IsInvisibleState(const entity_state_t *state)
{
	const int mode = g_Config.GetInvisible();

	if (mode <= 0) {
		return false;
	}

	if (state->effects & EF_NODRAW) {
		return true;
	}

	if (mode >= 2) {
		return state->rendermode != kRenderNormal;
	}

	if (state->renderamt != 0) {
		return false;
	}

	switch (state->rendermode)
	{
	case kRenderTransColor:
	case kRenderTransTexture:
	case kRenderTransAlpha:
	case kRenderTransAdd:
		return true;
	default:
		return false;
	}
}

static entity_state_t *FindEntityState(packet_entities_t *to, int number)
{
	for (int j = 0; j < to->num_entities; j++)
	{
		if (to->entities[j].number == number) {
			return to->entities + j;
		}
	}

	return nullptr;
}

static void DebugFollowState(edict_t *pHost, entity_state_t *state, entity_state_t *aimentState, edict_t *pEnt, int playerIndex, float diff, int alpha, bool applied)
{
	static float s_lastLog[4096] = {};

	const int followIndex = state->number;

	if (followIndex <= 0 || followIndex >= (int)(sizeof(s_lastLog) / sizeof(s_lastLog[0]))) {
		return;
	}

	const float now = gpGlobals->time;

	if (now - s_lastLog[followIndex] < 0.5f) {
		return;
	}

	s_lastLog[followIndex] = now;

	edict_t *pAiment = pEnt->v.aiment;
	edict_t *pOwner = (pAiment != nullptr) ? pAiment->v.owner : nullptr;

	SEM_PRINT("[SEMICLIP-DBG] host=%d follow=%d aiment=%d owner=%d resolved=%d applied=%d diff=%.1f alpha=%d net_rm=%d net_amt=%d net_fx=%d net_ef=%x net_solid=%d | aiment_net_rm=%d aiment_net_amt=%d | follow_ev rm=%d amt=%.0f fx=%d ef=%x | aiment_ev rm=%d amt=%.0f fx=%d ef=%x | aimowner_ev rm=%d amt=%.0f fx=%d ef=%x",
		EDICT_NUM(pHost), followIndex,
		(pAiment != nullptr) ? EDICT_NUM(pAiment) : 0,
		(pOwner != nullptr) ? EDICT_NUM(pOwner) : 0,
		playerIndex, applied ? 1 : 0, diff, alpha,
		state->rendermode, state->renderamt, state->renderfx, state->effects, (int)state->solid,
		(aimentState != nullptr) ? aimentState->rendermode : -1, (aimentState != nullptr) ? aimentState->renderamt : -1,
		(int)pEnt->v.rendermode, pEnt->v.renderamt, (int)pEnt->v.renderfx, (int)pEnt->v.effects,
		(pAiment != nullptr) ? (int)pAiment->v.rendermode : -1, (pAiment != nullptr) ? pAiment->v.renderamt : 0.0f, (pAiment != nullptr) ? (int)pAiment->v.renderfx : -1, (pAiment != nullptr) ? (int)pAiment->v.effects : 0,
		(pOwner != nullptr) ? (int)pOwner->v.rendermode : -1, (pOwner != nullptr) ? pOwner->v.renderamt : 0.0f, (pOwner != nullptr) ? (int)pOwner->v.renderfx : -1, (pOwner != nullptr) ? (int)pOwner->v.effects : 0);
}

int SV_CreatePacketEntities(IRehldsHook_SV_CreatePacketEntities *chain, sv_delta_t type, IGameClient *client, packet_entities_t *to, sizebuf_t *msg)
{
	edict_t *pHost = client->GetEdict();

	if (pHost->v.deadflag != DEAD_NO) {
		return chain->callNext(type, client, to, msg);
	}

	CGamePlayer *pPlayer = PLAYER_FOR_EDICT(pHost);
	CBasePlayer *pl = (CBasePlayer *)GET_PRIVATE(pHost);

	const int maxClients = g_GameData.GetMaxClients();
	const int transparency = g_Config.GetTransparency();
	const int effects = g_Config.GetEffects();
	const bool debug = (g_Config.GetDebug() != 0);

	for (int j = 0; j < to->num_entities; j++)
	{
		entity_state_t *state = to->entities + j;
		int ent = state->number;
		edict_t *pEnt = EDICT(ent);
		CGamePlayer *pObject = nullptr;
		int followAiment = 0;

		if (ent <= maxClients)
		{
			if (pEnt->v.deadflag != DEAD_NO || pEnt == pHost) {
				continue;
			}

			pObject = PLAYER_FOR_EDICT(pEnt);
		}
		else
		{
			if (pl && pl->m_iTeam == TERRORIST)
			{
				CGrenade *pGrenade = (CGrenade *)GET_PRIVATE(pEnt);

				if (*(CGrenade **)pGrenade == g_pVirtualGrenade && pGrenade->m_bIsC4)
				{
					state->solid = SOLID_NOT;
					continue;
				}
			}

			if (pEnt->v.aiment == nullptr
				|| pEnt->v.movetype != MOVETYPE_FOLLOW
				|| pEnt->v.aiment == pHost
				|| pEnt->v.rendermode != kRenderNormal)
			{
				continue;
			}

			edict_t *pAiment = pEnt->v.aiment;
			followAiment = EDICT_NUM(pAiment);
			int playerIndex = followAiment;

			if (playerIndex < 1 || playerIndex > maxClients)
			{
				edict_t *pOwner = pAiment->v.owner;

				if (pOwner != nullptr) {
					playerIndex = EDICT_NUM(pOwner);
				}
			}

			if (playerIndex < 1 || playerIndex > maxClients) {
				continue;
			}

			// a follow entity of a dead player should not be touched by semiclip
			if (EDICT(playerIndex)->v.deadflag != DEAD_NO) {
				continue;
			}

			pObject = PLAYER_FOR_NUM(playerIndex - 1);
		}

		if (pObject == nullptr || !pPlayer->GetSolid(pObject->GetIndex())) {
			continue;
		}

		const bool bInvisible = IsInvisibleState(state);
		const bool bNonPlayer = (ent > maxClients);

		state->solid = SOLID_NOT;

		if (transparency == 0 || bInvisible) {
			continue;
		}

		const float diff = pPlayer->GetDiff(pObject->GetIndex());
		int alpha;

		if (effects) {
			alpha = (diff > MIN_AMOUNT) ? (int)diff : (int)MIN_AMOUNT;
		}
		else {
			alpha = transparency;
		}

		// renderamt is transmitted as a single byte; never allow it to wrap to 0
		if (alpha < 1) {
			alpha = 1;
		}
		else if (alpha > 255) {
			alpha = 255;
		}

		// Players are drawn through the player pipeline where kRenderTransAlpha fades smoothly.
		// Non-player studio models (dance copies, hats) are drawn through the generic model path.
		state->rendermode = bNonPlayer ? kRenderTransTexture : kRenderTransAlpha;
		state->renderamt = alpha;

		// GoldSrc draws a follow entity in the transparent pass only when its aiment is ALSO
		// in the transparent list (see engine r_trans.cpp, R_DrawTEntitiesOnList). Otherwise the
		// copy is skipped entirely and pops out of existence. Put the aiment there too.
		entity_state_t *pAimentState = nullptr;

		if (bNonPlayer && followAiment > maxClients && followAiment != ent)
		{
			pAimentState = FindEntityState(to, followAiment);

			if (pAimentState != nullptr)
			{
				// If the aiment already has a custom render mode it is already in the
				// transparent list, so leave it untouched. Otherwise make it transparent
				// with alpha 0: it must be present in the transparent list so the client
				// draws the follow copy after borrowing its bones, but the aiment itself
				// must not be rendered (the dance animation model is made of additive
				// placeholder geometry and would show up as dark boxes).
				if (pAimentState->rendermode == kRenderNormal)
				{
					pAimentState->rendermode = kRenderTransTexture;
					pAimentState->renderamt = 0;
				}
			}
		}

		if (debug && bNonPlayer) {
			DebugFollowState(pHost, state, pAimentState, pEnt, pObject->GetIndex(), diff, alpha, true);
		}
	}

	return chain->callNext(type, client, to, msg);
}

void CBasePlayer_Spawn(IReGameHook_CBasePlayer_Spawn *chain, CBasePlayer *pthis)
{
	CGamePlayer *pPlayer = PLAYER_FOR_NUM(pthis->entindex() - 1);

	if (pPlayer != nullptr)
	{
		pPlayer->Player_Clear();
	}

	chain->callNext(pthis);
}

inline bool IsTeamAllowed(CBasePlayer *HostPlayer, CBasePlayer *EntPlayer)
{
	auto hostTeam = HostPlayer->m_iTeam;

	if (hostTeam == SPECTATOR || g_Config.GetTeam() == SC_TEAM_ALL)
		return true;

	if (g_Config.GetTeam() == SC_TEAM_TEAMMATE)
		return (CSGameRules()->PlayerRelationship(HostPlayer, EntPlayer) == GR_TEAMMATE);

	// SC_TEAM_T, SC_TEAM_CT
	return (hostTeam == g_Config.GetTeam() && EntPlayer->m_iTeam == g_Config.GetTeam());
}

inline bool allowDontSolid(playermove_t *pm, edict_t *pHost, int host, int j)
{
	pHost = EDICT(host);

	CBasePlayer *HostPlayer = (CBasePlayer *)CBaseEntity::Instance(pHost);

	if (!HostPlayer || !HostPlayer->IsPlayer()) {
		return false;
	}

	int ent = pm->physents[j].player;
	edict_t *pEntity = EDICT(ent);
	CBasePlayer *EntPlayer = (CBasePlayer *)CBaseEntity::Instance(pEntity);

	if (!EntPlayer || !EntPlayer->IsPlayer()) {
		return false;
	}

	CGamePlayer *pPlayer = PLAYER_FOR_NUM(host - 1);
	CGamePlayer *pObject = PLAYER_FOR_NUM(ent - 1);
	entvars_t *pevHost = &(pHost->v);
	entvars_t *pevEnt = &(pEntity->v);
	Vector hostOrigin = pevHost->origin;
	Vector entOrigin = pevEnt->origin;
	int IndexObject = pObject->GetIndex();
	int hostTeamId = HostPlayer->m_iTeam;
	int entTeamId = EntPlayer->m_iTeam;

	*pPlayer->GetDiff(pObject) = GET_DISTANCE(hostOrigin, entOrigin);
	*pPlayer->GetSolid(pObject) =
		(
			(g_Config.GetEffects() || *pPlayer->GetDiff(pObject) < g_Config.GetDistance()) && IsTeamAllowed(HostPlayer, EntPlayer) && !pObject->GetDont()
		);

	if (g_Config.GetCrouch() && pPlayer->GetSolid(IndexObject))
	{
		int IndexPlayer = pPlayer->GetIndex();
		float fDiff = abs(hostOrigin.z - entOrigin.z);

		if (fDiff < FLOAT_CROUCH
			&& pPlayer->GetCrouch(IndexObject)
			&& pObject->GetCrouch(IndexPlayer))
		{
			*pPlayer->GetCrouch(pObject) = false;
			*pObject->GetCrouch(pPlayer) = false;
		}

		if (fDiff >= FLOAT_CROUCH
			&& !pPlayer->GetCrouch(IndexObject)
			&& pevHost->button & IN_DUCK
			&& (pevEnt->button & IN_DUCK
			|| pevEnt->flags & FL_DUCKING))
		{
			*pPlayer->GetCrouch(pObject) = true;
			*pObject->GetCrouch(pPlayer) = true;
			return *pPlayer->GetSolid(pObject) = false;
		}
		else if ((pPlayer->GetCrouch(IndexObject)
				|| pObject->GetCrouch(IndexPlayer))
				&& (pevHost->groundentity == pEntity
				|| pevEnt->groundentity == pHost
				|| fDiff >= FLOAT_CROUCH))
		{
			return *pPlayer->GetSolid(pObject) = false;
		}
	}

	return pPlayer->GetSolid(IndexObject);
}

void PM_Move(playermove_t *pm, int server)
{
	if (pm->deadflag != DEAD_NO || pm->dead || pm->spectator) {
		RETURN_META(MRES_IGNORED);
	}

	int j, host, numphyspl = 0, numphysent = -1;
	host = pm->player_index + 1;

	edict_t *pHost = EDICT(host);
	CBasePlayer *pCBasePlayer = (CBasePlayer *)CBaseEntity::Instance(pHost);

	if (pCBasePlayer && pCBasePlayer->IsPlayer() && pCBasePlayer->m_iTeam == TERRORIST)
	{
		// to changes mins / maxs c4 from physents
		for (int i = 0; i < pm->numphysent; ++i)
		{
			CBaseEntity *pEntity = (CBaseEntity *)GET_PRIVATE(EDICT(pm->physents[i].info));
			CGrenade *pGrenade = (CGrenade *)pEntity;

			if (*(CGrenade **)pGrenade != g_pVirtualGrenade || !pGrenade->m_bIsC4) {
				continue;
			}

			pm->physents[i].maxs = {};
			pm->physents[i].mins = {};

			// it found
			break;
		}
	}

	for (j = 0; j < pm->numphysent; ++j)
	{
		if (pm->physents[++numphysent].player && ++numphyspl) {
			break;
		}
	}

	if (!numphyspl)
	{
		CGamePlayer *HostPlayer = PLAYER_FOR_NUM(host - 1);

		if (HostPlayer != nullptr)
		{
			HostPlayer->Player_Clear();
		}
	}

	for (j = numphysent; j < pm->numphysent; ++j)
	{
		if (!pm->physents[j].player || !allowDontSolid(pm, pHost, host, j))
		{
			pm->physents[numphysent++] = pm->physents[j];
		}
	}

	if (g_Config.GetTime() && gpGlobals->time > g_Config.GetCount())
	{
		if (!pCBasePlayer || !pCBasePlayer->IsPlayer())
		{
			pm->numphysent = numphysent;
			RETURN_META(MRES_IGNORED);
		}

		bool bCollide = false;
		bool needSolid = false;
		int hostTeamId = pCBasePlayer->m_iTeam;
		Vector hostOrigin = pHost->v.origin;
		CGamePlayer *pPlayer = PLAYER_FOR_NUM(host - 1);
		edict_t *pEntity;

		for (pEntity = g_GameData.GetStartEnt(), j = 1; pEntity <= g_GameData.GetMaxClientEdict(); pEntity++, j++)
		{
			if (!pEntity->pvPrivateData) {
				continue;
			}

			CGamePlayer *pObject = PLAYER_FOR_NUM(j - 1);

			if (pObject->GetDont()) {
				continue;
			}

			entvars_t *e = &(pEntity->v);

			if (e->deadflag != DEAD_NO || e->health <= 0.0) {
				continue;
			}

			if (!bCollide && j != host && GET_COLLIDE(hostOrigin, e->origin))
			{
				CBasePlayer *EntPlayer = (CBasePlayer *)CBaseEntity::Instance(pEntity);

				if (!EntPlayer || !EntPlayer->IsPlayer()) {
					continue;
				}

				if (IsTeamAllowed(pCBasePlayer, EntPlayer))
				{
					bCollide = true;
				}
			}

			needSolid = true;

			if (bCollide && needSolid) {
				break;
			}
		}

		if (!numphyspl || !bCollide) {
			pPlayer->SetDont(true);
		}

		if (!needSolid)
		{
			g_RehldsHookchains->SV_CreatePacketEntities()->unregisterHook(&SV_CreatePacketEntities);

			g_ReGameHookchains->CBasePlayer_Spawn()->unregisterHook(&CBasePlayer_Spawn);
			g_ReGameHookchains->CBasePlayer_TraceAttack()->unregisterHook(&CBasePlayer_TraceAttack);

			g_pFunctionTable->pfnPM_Move = nullptr;
			g_pNewFunctionTable->pfnShouldCollide = nullptr;
		}
	}

	pm->numphysent = numphysent;

	RETURN_META(MRES_IGNORED);
}

void RadiusFlash_TraceLine(IReGameHook_RadiusFlash_TraceLine *chain, CBasePlayer *pPlayer, entvars_t *pevInflictor, entvars_t *pevAttacker, Vector& vecSrc, Vector& vecSpot, TraceResult *ptr)
{
	chain->callNext(pPlayer, pevInflictor, pevAttacker, vecSrc, vecSpot, ptr);

	CGamePlayer *pHost = PLAYER_FOR_EDICT(pPlayer->edict());

	for (int i = 0; i < g_GameData.GetMaxClients(); i++)
	{
		edict_t *pHit = ptr->pHit;

		if (pHit < g_GameData.GetStartEnt() || pHit > g_GameData.GetMaxClientEdict()) {
			break;
		}

		CGamePlayer *pTarget = PLAYER_FOR_EDICT(pHit);

		if (!pHost->GetSolid(pTarget->GetIndex())) {
			break;
		}

		vecSrc = pHit->v.origin + pHit->v.view_ofs;

		TRACE_LINE(vecSrc, vecSpot, 0, pHit, ptr);
	}
}

void CSGameRules_OnRoundFreezeEnd(IReGameHook_CSGameRules_OnRoundFreezeEnd *chain)
{
	chain->callNext();

	ClearAllClients();

	g_Config.SetCount(gpGlobals->time + g_Config.GetTime());

	g_RehldsHookchains->SV_CreatePacketEntities()->unregisterHook(&SV_CreatePacketEntities);
	g_RehldsHookchains->SV_CreatePacketEntities()->registerHook(&SV_CreatePacketEntities, HC_PRIORITY_HIGH);

	g_ReGameHookchains->CBasePlayer_Spawn()->unregisterHook(&CBasePlayer_Spawn);
	g_ReGameHookchains->CBasePlayer_Spawn()->registerHook(&CBasePlayer_Spawn, HC_PRIORITY_DEFAULT + 1);

	if (g_Config.GetPenetFire())
	{
		g_ReGameHookchains->CBasePlayer_TraceAttack()->unregisterHook(&CBasePlayer_TraceAttack);
		g_ReGameHookchains->CBasePlayer_TraceAttack()->registerHook(&CBasePlayer_TraceAttack, HC_PRIORITY_DEFAULT + 1);
	}

	g_pFunctionTable->pfnPM_Move = PM_Move;
	g_pNewFunctionTable->pfnShouldCollide = ShouldCollide;
}

void SVR_SemiclipOption()
{
	if (CMD_ARGC() < 3)
	{
		Print_Settings();
		return;
	}

	const char *argv = CMD_ARGV(1);
	const char *value = CMD_ARGV(2);

	if (*value == '\0') {
		return;
	}

	if (!g_Config.Parse_Settings(argv, value)) {
		Print_Settings();
	}
	else if (!g_Config.GetEnable())
	{
		g_RehldsHookchains->SV_CreatePacketEntities()->unregisterHook(&SV_CreatePacketEntities);

		g_ReGameHookchains->CBasePlayer_Spawn()->unregisterHook(&CBasePlayer_Spawn);
		g_ReGameHookchains->CBasePlayer_TraceAttack()->unregisterHook(&CBasePlayer_TraceAttack);
		g_ReGameHookchains->CSGameRules_OnRoundFreezeEnd()->unregisterHook(&CSGameRules_OnRoundFreezeEnd);

		g_pFunctionTable->pfnPM_Move = nullptr;
		g_pNewFunctionTable->pfnShouldCollide = nullptr;
	}
	else
	{
		if (!strcasecmp(argv, "semiclip"))
		{
			ClearAllClients();

			g_RehldsHookchains->SV_CreatePacketEntities()->unregisterHook(&SV_CreatePacketEntities);
			g_RehldsHookchains->SV_CreatePacketEntities()->registerHook(&SV_CreatePacketEntities, HC_PRIORITY_HIGH);

			g_ReGameHookchains->CBasePlayer_Spawn()->unregisterHook(&CBasePlayer_Spawn);
			g_ReGameHookchains->CBasePlayer_Spawn()->registerHook(&CBasePlayer_Spawn, HC_PRIORITY_DEFAULT + 1);
			g_ReGameHookchains->CSGameRules_OnRoundFreezeEnd()->unregisterHook(&CSGameRules_OnRoundFreezeEnd);

			if (g_Config.GetPenetFire())
			{
				g_ReGameHookchains->CBasePlayer_TraceAttack()->unregisterHook(&CBasePlayer_TraceAttack);
				g_ReGameHookchains->CBasePlayer_TraceAttack()->registerHook(&CBasePlayer_TraceAttack, HC_PRIORITY_DEFAULT + 1);
			}

			g_pFunctionTable->pfnPM_Move = PM_Move;
			g_pNewFunctionTable->pfnShouldCollide = ShouldCollide;
		}
		else if (!strcasecmp(argv, "time"))
		{
			if (g_Config.GetTime()) {
				g_ReGameHookchains->CSGameRules_OnRoundFreezeEnd()->registerHook(&CSGameRules_OnRoundFreezeEnd, HC_PRIORITY_DEFAULT + 1);
			}
		}
		else if (!strcasecmp(argv, "penetfire"))
		{
			if (g_Config.GetPenetFire()) {
				g_ReGameHookchains->CBasePlayer_TraceAttack()->unregisterHook(&CBasePlayer_TraceAttack);
				g_ReGameHookchains->CBasePlayer_TraceAttack()->registerHook(&CBasePlayer_TraceAttack, HC_PRIORITY_DEFAULT + 1);
			}
			else
			{
				g_ReGameHookchains->CBasePlayer_TraceAttack()->unregisterHook(&CBasePlayer_TraceAttack);
			}
		}
	}
}

class CGroupMask
{

public:

	CGroupMask(CBasePlayer *pAttacker)
	{
		m_pev = pAttacker->pev;
		m_iTeam = pAttacker->m_iTeam;
		m_iIndex = pAttacker->entindex();
		m_pAttacker = PLAYER_FOR_EDICT(pAttacker->edict());
	}

	void SetGroupMask()
	{
		for (int i = 1; i <= g_GameData.GetMaxClients(); ++i)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);

			if (!pPlayer || pPlayer->m_iTeam != m_iTeam || i == m_iIndex) {
				continue;
			}

			CGamePlayer *pGamePlayer = PLAYER_FOR_EDICT(pPlayer->edict());

			if (pGamePlayer && pGamePlayer->GetSolid(m_iIndex))
			{
				pPlayer->pev->groupinfo |= (1 << m_iIndex) | (1 << i);
			}

			if (m_pAttacker->GetSolid(i))
			{
				m_pev->groupinfo |= (1 << m_iIndex) | (1 << i);
			}
		}

		ENGINE_SETGROUPMASK(0, GROUP_OP_NAND);
	}

	void ResetGroupMask()
	{
		for (int i = 1; i <= g_GameData.GetMaxClients(); ++i)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);

			if (!pPlayer || pPlayer->m_iTeam != m_iTeam || i == m_iIndex) {
				continue;
			}

			pPlayer->pev->groupinfo &= ~((1 << i) | (1 << m_iIndex));
			m_pev->groupinfo &= ~((1 << i) | (1 << m_iIndex));
		}

		ENGINE_SETGROUPMASK(0, GROUP_OP_AND);
	}

private:

	CGamePlayer *m_pAttacker;
	entvars_t *m_pev;
	int m_iTeam;
	int m_iIndex;

};

void CBasePlayer_TraceAttack(IReGameHook_CBasePlayer_TraceAttack *chain, CBasePlayer *pthis, entvars_t *pevAttacker, float flDamage, Vector& vecDir, TraceResult *ptr, int bitsDamageType)
{
	CBasePlayer *pAttacker((CBasePlayer *)CBaseEntity::Instance(pevAttacker));

	if (CSGameRules()->FPlayerCanTakeDamage(pthis, pAttacker)) {
		chain->callNext(pthis, pevAttacker, flDamage, vecDir, ptr, bitsDamageType);
		return;
	}

	const float flForward = (pAttacker->m_pActiveItem && pAttacker->m_pActiveItem->m_iId == WEAPON_KNIFE) ? 48.0f : 8192.0f;
	TraceResult tr;
	Vector vecStart(pAttacker->pev->origin + pAttacker->pev->view_ofs);
	Vector vecEnd(vecStart + vecDir * flForward);
	CGroupMask mask(pAttacker);

	mask.SetGroupMask();

	UTIL_TraceLine(vecStart, vecEnd, dont_ignore_monsters, pAttacker->edict(), &tr);

	mask.ResetGroupMask();

	if (tr.flFraction == 1.0f) {
		return;
	}

	CBasePlayer *pHit = (CBasePlayer *)CBaseEntity::Instance(tr.pHit);

	if (pHit != nullptr)
	{
		if (pHit->IsPlayer()) {
			chain->callNext(pHit, pevAttacker, flDamage, vecDir, &tr, bitsDamageType);
		}
		else
		{
			chain->callNext(pthis, pevAttacker, flDamage, vecDir, ptr, bitsDamageType);
		}
	}
}

int ShouldCollide(edict_t *pentTouched, edict_t *pentOther)
{
	CBaseEntity *pTouched = (CBaseEntity *)GET_PRIVATE(pentTouched);
	CBaseEntity *pOther = (CBaseEntity *)GET_PRIVATE(pentOther);

	if (pOther && pTouched && pTouched->IsPlayer())
	{
		// ignore owner's
		if (pentTouched->v.owner == pentOther || pentOther->v.owner == pentTouched) {
			RETURN_META_VALUE(MRES_IGNORED, 0);
		}

		if (!((pTouched->pev->flags | pOther->pev->flags) & FL_KILLME))
		{
			// if collided grenade / c4
			CGrenade *pGrenadeOther = (CGrenade *)pOther;

			if (*(CGrenade **)pGrenadeOther == g_pVirtualGrenade && pGrenadeOther->pev->owner)
			{
				if (pGrenadeOther->m_bIsC4 && ((CBasePlayer *)pTouched)->m_iTeam == TERRORIST) {
					RETURN_META_VALUE(MRES_SUPERCEDE, 0);
				}

				CGamePlayer *pGamePlayer = PLAYER_FOR_EDICT(pGrenadeOther->pev->owner);

				if (pGamePlayer && pGamePlayer->GetSolid(EDICT_NUM(pentTouched))) {
					RETURN_META_VALUE(MRES_SUPERCEDE, 0);
				}
			}
		}
	}

	RETURN_META_VALUE(MRES_IGNORED, 0);
}
