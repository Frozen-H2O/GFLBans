/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023 Source2ZE
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "protobuf/generated/usermessages.pb.h"

#include "adminsystem.h"
#include "KeyValues.h"
#include "interfaces/interfaces.h"
#include "icvar.h"
#include "playermanager.h"
#include "commands.h"
#include "ctimer.h"
#include "detours.h"
#include "discord.h"
#include "utils/entity.h"
#include "entity/cbaseentity.h"
#include "entity/cparticlesystem.h"
#include "entity/cgamerules.h"
#include "gamesystem.h"
#include <vector>
#include <cmath>
#include <ctime>
#include <map>
#include <memory>
#include <utility>
#include "httpmanager.h"
#include "vendor/nlohmann/json.hpp"
using json = nlohmann::json;

extern IVEngineServer2 *g_pEngineServer2;
extern CGameEntitySystem *g_pEntitySystem;
extern CGlobalVars *gpGlobals;
extern CCSGameRules *g_pGameRules;

CAdminSystem* g_pAdminSystem = nullptr;

CUtlMap<uint32, CChatCommand *> g_CommandList(0, 0, DefLessFunc(uint32));

static std::string g_sBeaconParticle = "particles/testsystems/test_cross_product.vpcf";

FAKE_STRING_CVAR(cs2f_admin_beacon_particle, ".vpcf file to be precached and used for admin beacon", g_sBeaconParticle, false)

void PrintSingleAdminAction(const char* pszAdminName, const char* pszTargetName, const char* pszAction, const char* pszAction2 = "", const char* prefix = CHAT_PREFIX)
{
	ClientPrintAll(HUD_PRINTTALK, "%s" ADMIN_PREFIX "%s %s%s.", prefix, pszAdminName, pszAction, pszTargetName, pszAction2);
}

void PrintMultiAdminAction(ETargetType nType, const char* pszAdminName, const char* pszAction, const char* pszAction2 = "", const char* prefix = CHAT_PREFIX)
{
	switch (nType)
	{
	case ETargetType::ALL:
		ClientPrintAll(HUD_PRINTTALK, "%s" ADMIN_PREFIX "%s everyone%s.", prefix, pszAdminName, pszAction, pszAction2);
		break;
	case ETargetType::T:
		ClientPrintAll(HUD_PRINTTALK, "%s" ADMIN_PREFIX "%s terrorists%s.", prefix, pszAdminName, pszAction, pszAction2);
		break;
	case ETargetType::CT:
		ClientPrintAll(HUD_PRINTTALK, "%s" ADMIN_PREFIX "%s counter-terrorists%s.", prefix, pszAdminName, pszAction, pszAction2);
		break;
	}
}

CON_COMMAND_F(c_reload_admins, "Reload admin config", FCVAR_SPONLY | FCVAR_LINKED_CONCOMMAND)
{
	if (!g_pAdminSystem->LoadAdmins())
		return;

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		ZEPlayer* pPlayer = g_playerManager->GetPlayer(i);

		if (!pPlayer || pPlayer->IsFakeClient() || !pPlayer->IsAuthenticated())
			continue;

		pPlayer->CheckAdmin();
	}

	Message("Admins reloaded\n");
}

CON_COMMAND_F(c_reload_infractions, "Reload infractions to sync with GFLBans", FCVAR_SPONLY | FCVAR_LINKED_CONCOMMAND)
{
	g_pAdminSystem->RemoveAllPunishments();
	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		ZEPlayer* pPlayer = g_playerManager->GetPlayer(i);

		if (!pPlayer || pPlayer->IsFakeClient())
			continue;

		pPlayer->CheckInfractions();
	}

	Message("Infractions queries sent to GFLBans\n");
}

CON_COMMAND_CHAT_FLAGS(ban, "<name> <duration> <reason> - ban a player", ADMFLAG_BAN)
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !ban <name> <duration> <reason>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlot[MAXPLAYERS];

	if (g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlot) != ETargetType::PLAYER || iNumClients > 1)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"You can only target individual players for banning.");
		return;
	}

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Target not found.");
		return;
	}

	int iDuration = ParseTimeInput(args[2]);

	if (iDuration == -1)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Invalid duration.");
		return;
	}
	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[0]);

	if (!pTarget)
		return;

	ZEPlayer* pTargetPlayer = g_playerManager->GetPlayer(pSlot[0]);

	if (pTargetPlayer->IsFakeClient())
		return;

	if (!pTargetPlayer->IsAuthenticated())
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is not yet authenticated, consider kicking instead or please wait a moment and try again.", pTarget->GetPlayerName());
		return;
	}

	std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson = std::make_shared<GFLBans_PlayerObjSimple>(pTargetPlayer);
	std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin = nullptr;
	if (player)
		plyAdmin = std::make_shared<GFLBans_PlayerObjNoIp>(player->GetZEPlayer());
	std::string strReason = "";
	for (int i = 3; i < args.ArgC(); i++)
		strReason = strReason + args[i] + " ";
	if (strReason.length() > 0)
		strReason = strReason.substr(0, strReason.length() - 1);

	std::shared_ptr<GFLBans_Infraction> infraction = std::make_shared<GFLBans_Infraction>(
		plyBadPerson, GFLBans_InfractionBase::GFLInfractionType::Ban, strReason, plyAdmin, iDuration);

	g_pAdminSystem->GFLBans_CreateInfraction(infraction, pTargetPlayer, player);
}

CON_COMMAND_CHAT_FLAGS(mute, "<name> <(+)duration> <reason> - mutes a player", ADMFLAG_CHAT)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !mute <name> <(+)duration> <reason>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlot[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlot);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	int iDuration = args.ArgC() < 3 ? -1 : ParseTimeInput(args[2]);

	if (iDuration == 0 && nType >= ETargetType::ALL)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You may only permanently mute individuals.");
		return;
	}
	
	if (nType == ETargetType::RANDOM || nType == ETargetType::RANDOM_T || nType == ETargetType::RANDOM_CT)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You may not mute random players.");
		return;
	}

	if (iNumClients != 1)
	{
	// Targetting a group of people. Do not log these on GFLBans.
		for (int i = 0; i < iNumClients; i++)
		{
			CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[i]);

			if (!pTarget)
				continue;

			ZEPlayer* pTargetPlayer = g_playerManager->GetPlayer(pSlot[i]);

			if (pTargetPlayer->IsFakeClient())
				continue;

			CInfractionBase* infraction = new CMuteInfraction(iDuration < 0 ? 0 : iDuration,
															  pTargetPlayer->GetSteamId64(),
															  false, true);

			// We're overwriting the infraction, so remove the previous one first
			g_pAdminSystem->FindAndRemoveInfraction(pTargetPlayer, CInfractionBase::Mute);
			g_pAdminSystem->AddInfraction(infraction);
			infraction->ApplyInfraction(pTargetPlayer);
			if (iDuration > 0)
			// Only run this once since we simply dont need to heartbeat it. We know when the punishment ends
				g_pAdminSystem->RemoveSessionPunishments(iDuration * 60.0 + 1);
		}
		char szAction[64];
		V_snprintf(szAction, sizeof(szAction), " for %s", FormatTime(iDuration, false).c_str());
		const char* pszCommandPlayerName = player ? player->GetPlayerName() : "Console";
		PrintMultiAdminAction(nType, pszCommandPlayerName, "muted", szAction);
	}
	else
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[0]);

		if (!pTarget)
			return;

		ZEPlayer* pTargetPlayer = g_playerManager->GetPlayer(pSlot[0]);

		if (pTargetPlayer->IsFakeClient())
			return;

		if (!pTargetPlayer->IsAuthenticated())
		{
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is not yet authenticated, please wait a moment and try again.", pTarget->GetPlayerName());
			return;
		}

		std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson = std::make_shared<GFLBans_PlayerObjSimple>(pTargetPlayer);
		std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin = nullptr;
		if (player)
			plyAdmin = std::make_shared<GFLBans_PlayerObjNoIp>(player->GetZEPlayer());

		std::string strReason = "";
		for (int i = 3; i < args.ArgC(); i++)
			strReason = strReason + args[i] + " ";
		if (strReason.length() > 0)
			strReason = strReason.substr(0, strReason.length() - 1);

		bool bOnlineOnly = iDuration > 0 && (!g_pAdminSystem->CanPunishmentBeOffline(iDuration) || 
											 args[2][0] == '+');

		std::shared_ptr<GFLBans_Infraction> infraction = std::make_shared<GFLBans_Infraction>(
			plyBadPerson, GFLBans_InfractionBase::GFLInfractionType::Mute, strReason, plyAdmin,
			iDuration, bOnlineOnly);

		g_pAdminSystem->GFLBans_CreateInfraction(infraction, pTargetPlayer, player);
	}
}

CON_COMMAND_CHAT_FLAGS(unmute, "<name> <reason> - unmutes a player", ADMFLAG_CHAT)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !unmute <name> <reason>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlot[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlot);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	if (nType == ETargetType::RANDOM || nType == ETargetType::RANDOM_T || nType == ETargetType::RANDOM_CT)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You may not unmute random players.");
		return;
	}

	if (iNumClients != 1)
	{
		for (int i = 0; i < iNumClients; i++)
		{
			CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[i]);

			if (!pTarget)
				continue;

			ZEPlayer *pTargetPlayer = g_playerManager->GetPlayer(pSlot[i]);

			if (pTargetPlayer->IsFakeClient())
				continue;

			if (g_pAdminSystem->FindAndRemoveInfraction(pTargetPlayer, CInfractionBase::Mute))
			// Prevent players with web mutes from speaking after a mass ummute
				g_pAdminSystem->GFLBans_CheckPlayerInfractions(pTargetPlayer);
		}

		const char* pszCommandPlayerName = player ? player->GetPlayerName() : "Console";
		PrintMultiAdminAction(nType, pszCommandPlayerName, "unmuted");
	}
	else
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[0]);

		if (!pTarget)
			return;

		ZEPlayer* pTargetPlayer = g_playerManager->GetPlayer(pSlot[0]);

		if (pTargetPlayer->IsFakeClient())
			return;

		if (!pTargetPlayer->IsAuthenticated())
		{
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is not yet authenticated, please wait a moment and try again.", pTarget->GetPlayerName());
			return;
		}

		std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson = std::make_shared<GFLBans_PlayerObjSimple>(pTargetPlayer);
		std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin = nullptr;
		if (player)
			plyAdmin = std::make_shared<GFLBans_PlayerObjNoIp>(player->GetZEPlayer());
		std::string strReason = "";
		for (int i = 2; i < args.ArgC(); i++)
			strReason = strReason + args[i] + " ";
		if (strReason.length() > 0)
			strReason = strReason.substr(0, strReason.length() - 1);

		std::shared_ptr<GFLBans_RemoveInfractionsOfPlayer> infraction = std::make_shared<GFLBans_RemoveInfractionsOfPlayer>(
			plyBadPerson, GFLBans_InfractionBase::GFLInfractionType::Mute, strReason, plyAdmin);

		g_pAdminSystem->GFLBans_RemoveInfraction(infraction, pTargetPlayer, player);
	}
}

CON_COMMAND_CHAT_FLAGS(gag, "<name> <(+)duration> <reason> - gag a player", ADMFLAG_CHAT)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !gag <name> <(+)duration> <reason>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlot[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlot);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	int iDuration = args.ArgC() < 3 ? -1 : ParseTimeInput(args[2]);

	if (iDuration == 0 && nType >= ETargetType::ALL)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You may only permanently gag individuals.");
		return;
	}

	if (nType == ETargetType::RANDOM || nType == ETargetType::RANDOM_T || nType == ETargetType::RANDOM_CT)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You may not gag random players.");
		return;
	}

	if (iNumClients != 1)
	{
		for (int i = 0; i < iNumClients; i++)
		{
			CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[i]);

			if (!pTarget)
				continue;

			ZEPlayer *pTargetPlayer = g_playerManager->GetPlayer(pSlot[i]);

			if (pTargetPlayer->IsFakeClient())
				continue;

			CInfractionBase* infraction = new CGagInfraction(iDuration < 0 ? 0 : iDuration,
															 pTargetPlayer->GetSteamId64(),
															 false, true);

			// We're overwriting the infraction, so remove the previous one first
			g_pAdminSystem->FindAndRemoveInfraction(pTargetPlayer, CInfractionBase::Gag);
			g_pAdminSystem->AddInfraction(infraction);
			infraction->ApplyInfraction(pTargetPlayer);
			if (iDuration > 0)
			// Only run this once since we simply dont need to heartbeat it. We know when the punishment ends
				g_pAdminSystem->RemoveSessionPunishments(iDuration * 60.0 + 1);
		}
		char szAction[64];
		V_snprintf(szAction, sizeof(szAction), " for %s", FormatTime(iDuration, false).c_str());
		const char* pszCommandPlayerName = player ? player->GetPlayerName() : "Console";
		PrintMultiAdminAction(nType, pszCommandPlayerName, "gagged", szAction);
	}
	else
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[0]);

		if (!pTarget)
			return;

		ZEPlayer* pTargetPlayer = g_playerManager->GetPlayer(pSlot[0]);

		if (pTargetPlayer->IsFakeClient())
			return;

		if (!pTargetPlayer->IsAuthenticated())
		{
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is not yet authenticated, please wait a moment and try again.", pTarget->GetPlayerName());
			return;
		}

		std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson = std::make_shared<GFLBans_PlayerObjSimple>(pTargetPlayer);
		std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin = nullptr;
		if (player)
			plyAdmin = std::make_shared<GFLBans_PlayerObjNoIp>(player->GetZEPlayer());
		std::string strReason = "";
		for (int i = 3; i < args.ArgC(); i++)
			strReason = strReason + args[i] + " ";
		if (strReason.length() > 0)
			strReason = strReason.substr(0, strReason.length() - 1);

		bool bOnlineOnly = iDuration > 0 && (args[2][0] == '+' ||
											 !g_pAdminSystem->CanPunishmentBeOffline(iDuration));

		std::shared_ptr<GFLBans_Infraction> infraction = std::make_shared<GFLBans_Infraction>(
			plyBadPerson, GFLBans_InfractionBase::GFLInfractionType::Gag, strReason, plyAdmin,
			iDuration, bOnlineOnly);

		g_pAdminSystem->GFLBans_CreateInfraction(infraction, pTargetPlayer, player);
	}
}

CON_COMMAND_CHAT_FLAGS(ungag, "<name> <reason> - ungags a player", ADMFLAG_CHAT)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !ungag <name> <reason>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlot[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlot);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	if (nType == ETargetType::RANDOM || nType == ETargetType::RANDOM_T || nType == ETargetType::RANDOM_CT)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You may not ungag random players.");
		return;
	}

	if (iNumClients != 1)
	{
		for (int i = 0; i < iNumClients; i++)
		{
			CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[i]);

			if (!pTarget)
				continue;

			ZEPlayer *pTargetPlayer = g_playerManager->GetPlayer(pSlot[i]);

			if (pTargetPlayer->IsFakeClient())
				continue;

			if (g_pAdminSystem->FindAndRemoveInfraction(pTargetPlayer, CInfractionBase::Gag))
			// Prevent players with web mutes from typing after a mass ungag
				g_pAdminSystem->GFLBans_CheckPlayerInfractions(pTargetPlayer);
		}

		const char* pszCommandPlayerName = player ? player->GetPlayerName() : "Console";
		PrintMultiAdminAction(nType, pszCommandPlayerName, "ungagged");
	}
	else
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[0]);

		if (!pTarget)
			return;

		ZEPlayer* pTargetPlayer = g_playerManager->GetPlayer(pSlot[0]);

		if (pTargetPlayer->IsFakeClient())
			return;

		if (!pTargetPlayer->IsAuthenticated())
		{
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is not yet authenticated, please wait a moment and try again.", pTarget->GetPlayerName());
			return;
		}

		std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson = std::make_shared<GFLBans_PlayerObjSimple>(pTargetPlayer);
		std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin = nullptr;
		if (player)
			plyAdmin = std::make_shared<GFLBans_PlayerObjNoIp>(player->GetZEPlayer());
		std::string strReason = "";
		for (int i = 2; i < args.ArgC(); i++)
			strReason = strReason + args[i] + " ";
		if (strReason.length() > 0)
			strReason = strReason.substr(0, strReason.length() - 1);

		std::shared_ptr<GFLBans_RemoveInfractionsOfPlayer> infraction = std::make_shared<GFLBans_RemoveInfractionsOfPlayer>(
			plyBadPerson, GFLBans_InfractionBase::GFLInfractionType::Gag, strReason, plyAdmin);

		g_pAdminSystem->GFLBans_RemoveInfraction(infraction, pTargetPlayer, player);
	}
}

CON_COMMAND_CHAT_FLAGS(kick, "<name> - kick a player", ADMFLAG_KICK)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !kick <name>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlot[MAXPLAYERS];
	ENetworkDisconnectionReason dcReason = NETWORK_DISCONNECT_KICKED;
	if (args.ArgC() > 2)
	{
		std::string strReason = args[2];
		std::transform(strReason.begin(), strReason.end(), strReason.begin(),
					   [](unsigned char c) { return std::tolower(c); });
		if (strReason == "afk")
			dcReason = NETWORK_DISCONNECT_KICKED_IDLE;
	}

	g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlot);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Target not found.");
		return;
	}

	const char *pszCommandPlayerName = player ? player->GetPlayerName() : "Console";

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[i]);

		if (!pTarget)
			continue;

		ZEPlayer* pTargetPlayer = g_playerManager->GetPlayer(pSlot[i]);

		g_pEngineServer2->DisconnectClient(pTargetPlayer->GetPlayerSlot(), dcReason);

		PrintSingleAdminAction(pszCommandPlayerName, pTarget->GetPlayerName(), "kicked");
	}
}

CON_COMMAND_CHAT_FLAGS(slay, "<name> - slay a player", ADMFLAG_SLAY)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !slay <name>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlots);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Target not found.");
		return;
	}

	const char *pszCommandPlayerName = player ? player->GetPlayerName() : "Console";

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		if (!pTarget)
			continue;

		pTarget->GetPawn()->CommitSuicide(false, true);

		if (nType < ETargetType::ALL)
			PrintSingleAdminAction(pszCommandPlayerName, pTarget->GetPlayerName(), "slayed");
	}

	PrintMultiAdminAction(nType, pszCommandPlayerName, "slayed");
}

CON_COMMAND_CHAT_FLAGS(slap, "<name> [damage] - slap a player", ADMFLAG_SLAY)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !slap <name> <optional damage>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlots);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	const char *pszCommandPlayerName = player ? player->GetPlayerName() : "Console";

	for (int i = 0; i < iNumClients; i++)
	{
		CBasePlayerController *pTarget = (CBasePlayerController *)g_pEntitySystem->GetBaseEntity((CEntityIndex)(pSlots[i] + 1));

		if (!pTarget)
			continue;

		CBasePlayerPawn *pPawn = pTarget->m_hPawn();

		if (!pPawn)
			continue;

		// Taken directly from sourcemod
		Vector velocity = pPawn->m_vecAbsVelocity;
		velocity.x += ((rand() % 180) + 50) * (((rand() % 2) == 1) ? -1 : 1);
		velocity.y += ((rand() % 180) + 50) * (((rand() % 2) == 1) ? -1 : 1);
		velocity.z += rand() % 200 + 100;
		pPawn->SetAbsVelocity(velocity);

		int iDamage = V_StringToInt32(args[2], 0);

		if (iDamage > 0)
			pPawn->TakeDamage(iDamage);

		if (nType < ETargetType::ALL)
			PrintSingleAdminAction(pszCommandPlayerName, pTarget->GetPlayerName(), "slapped");
	}

	PrintMultiAdminAction(nType, pszCommandPlayerName, "slapped");
}

CON_COMMAND_CHAT_FLAGS(goto, "<name> - teleport to a player", ADMFLAG_SLAY)
{
	// Only players can use this command at all
	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You cannot use this command from the server console.");
		return;
	}

	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !goto <name>");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (g_playerManager->TargetPlayerString(player->GetPlayerSlot(), args[1], iNumClients, pSlots) != ETargetType::PLAYER || iNumClients > 1)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target too ambiguous.");
		return;
	}

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		if (!pTarget)
			continue;

		Vector newOrigin = pTarget->GetPawn()->GetAbsOrigin();

		player->GetPawn()->Teleport(&newOrigin, nullptr, nullptr);

		PrintSingleAdminAction(player->GetPlayerName(), pTarget->GetPlayerName(), "teleported to");
	}
}

CON_COMMAND_CHAT_FLAGS(bring, "<name> - bring a player", ADMFLAG_SLAY)
{
	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "You cannot use this command from the server console.");
		return;
	}

	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !bring <name>");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(player->GetPlayerSlot(), args[1], iNumClients, pSlots);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		if (!pTarget)
			continue;

		Vector newOrigin = player->GetPawn()->GetAbsOrigin();

		pTarget->GetPawn()->Teleport(&newOrigin, nullptr, nullptr);

		if (nType < ETargetType::ALL)
			PrintSingleAdminAction(player->GetPlayerName(), pTarget->GetPlayerName(), "brought");
	}

	PrintMultiAdminAction(nType, player->GetPlayerName(), "brought");
}

CON_COMMAND_CHAT_FLAGS(bringct, "<name> - bring a human", ADMFLAG_SLAY)
{
	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "You cannot use this command from the server console.");
		return;
	}

	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !bringct <name>");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(player->GetPlayerSlot(), args[1], iNumClients, pSlots);

	if (!iNumClients || nType == ETargetType::T || nType == ETargetType::RANDOM_T)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	bool bTPedPlayer = false;
	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		if (!pTarget || pTarget->m_iTeamNum() != CS_TEAM_CT)
			continue;

		Vector newOrigin = player->GetPawn()->GetAbsOrigin();

		pTarget->GetPawn()->Teleport(&newOrigin, nullptr, nullptr);
		bTPedPlayer = true;

		if (nType < ETargetType::ALL)
			PrintSingleAdminAction(player->GetPlayerName(), pTarget->GetPlayerName(), "brought");
	}

	if (!bTPedPlayer)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	PrintMultiAdminAction(nType, player->GetPlayerName(), "brought");
}

CON_COMMAND_CHAT_FLAGS(setteam, "<name> <team (0-3)> - set a player's team", ADMFLAG_SLAY)
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !setteam <name> <team (0-3)>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlots);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	int iTeam = V_StringToInt32(args[2], -1);

	if (iTeam < CS_TEAM_NONE || iTeam > CS_TEAM_CT)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Invalid team specified, range is 0-3.");
		return;
	}

	const char *pszCommandPlayerName = player ? player->GetPlayerName() : "Console";

	constexpr const char *teams[] = {"none", "spectators", "terrorists", "counter-terrorists"};

	char szAction[64];
	V_snprintf(szAction, sizeof(szAction), " to %s", teams[iTeam]);

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		if (!pTarget)
			continue;

		pTarget->SwitchTeam(iTeam);

		if (nType < ETargetType::ALL)
			PrintSingleAdminAction(pszCommandPlayerName, pTarget->GetPlayerName(), "moved", szAction);
	}

	PrintMultiAdminAction(nType, pszCommandPlayerName, "moved", szAction);
}

CON_COMMAND_CHAT_FLAGS(noclip, "- toggle noclip on yourself", ADMFLAG_SLAY | ADMFLAG_CHEATS)
{
	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "You cannot use this command from the server console.");
		return;
	}

	CBasePlayerPawn *pPawn = player->m_hPawn();

	if (!pPawn)
		return;

	if (pPawn->m_iHealth() <= 0)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You cannot noclip while dead!");
		return;
	}

	if (pPawn->m_nActualMoveType() == MOVETYPE_NOCLIP)
	{
		pPawn->SetMoveType(MOVETYPE_WALK);
		ClientPrintAll(HUD_PRINTTALK, CHAT_PREFIX ADMIN_PREFIX "exited noclip.", player->GetPlayerName());
	}
	else
	{
		pPawn->SetMoveType(MOVETYPE_NOCLIP);
		ClientPrintAll(HUD_PRINTTALK, CHAT_PREFIX ADMIN_PREFIX "entered noclip.", player->GetPlayerName());
	}
}

CON_COMMAND_CHAT_FLAGS(reload_discord_bots, "- Reload discord bot config", ADMFLAG_ROOT)
{
	g_pDiscordBotManager->LoadDiscordBotsConfig();
	Message("Discord bot config reloaded\n");
}

CON_COMMAND_CHAT_FLAGS(entfire, "<name> <input> [parameter] - fire outputs at entities", ADMFLAG_RCON)
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !entfire <name> <input> <optional parameter>");
		return;
	}

	int iFoundEnts = 0;

	Z_CBaseEntity *pTarget = nullptr;

	// The idea here is to only use one of the targeting modes at once, prioritizing !picker then targetname/!self then classname
	// Try picker first, FindEntityByName can also take !picker but it always uses player 0 so we have to do this ourselves
	if (player && !V_strcmp("!picker", args[1]))
	{
		pTarget = UTIL_FindPickerEntity(player);

		if (pTarget)
		{
			pTarget->AcceptInput(args[2], args[3], player, player);
			iFoundEnts++;
		}
	}

	// !self would resolve to the player controller, so here's a convenient alias to get the pawn instead
	if (player && !V_strcmp("!selfpawn", args[1]))
	{
		pTarget = player->GetPawn();

		if (pTarget)
		{
			pTarget->AcceptInput(args[2], args[3], player, player);
			iFoundEnts++;
		}
	}

	if (!iFoundEnts)
	{
		while (pTarget = UTIL_FindEntityByName(pTarget, args[1], player))
		{
			pTarget->AcceptInput(args[2], args[3], player, player);
			iFoundEnts++;
		}
	}

	if (!iFoundEnts)
	{
		while (pTarget = UTIL_FindEntityByClassname(pTarget, args[1]))
		{
			pTarget->AcceptInput(args[2], args[3], player, player);
			iFoundEnts++;
		}
	}

	if (!iFoundEnts)
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
	else
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Input successful on %i entities.", iFoundEnts);
}

CON_COMMAND_CHAT_FLAGS(entfirepawn, "<name> <inpu> [parameter] - fire outputs at player pawns", ADMFLAG_RCON)
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !entfirepawn <name> <input> <optional parameter>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlots);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	int iFoundEnts = 0;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController *pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		if (!pTarget || !pTarget->GetPawn())
			continue;

		pTarget->GetPawn()->AcceptInput(args[2], args[3], player, player);
		iFoundEnts++;
	}

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Input successful on %i player pawns.", iFoundEnts);
}

CON_COMMAND_CHAT_FLAGS(entfirecontroller, "<name> <input> [parameter] - fire outputs at player controllers", ADMFLAG_RCON)
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !entfirecontroller <name> <input> <optional parameter>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlots);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	int iFoundEnts = 0;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController *pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		if (!pTarget)
			continue;

		pTarget->AcceptInput(args[2], args[3], player, player);
		iFoundEnts++;
	}

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Input successful on %i player controllers.", iFoundEnts);
}

CON_COMMAND_CHAT_FLAGS(map, "<mapname> - change map", ADMFLAG_CHANGEMAP)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Usage: !map <mapname>");
		return;
	}

	if (!g_pEngineServer2->IsMapValid(args[1]))
	{
		// This might be a workshop map, and right now there's no easy way to get the list from a collection
		// So blindly attempt the change for now, as the command does nothing if the map isn't found
		std::string sCommand = "ds_workshop_changelevel " + std::string(args[1]);

		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Attempting a map change to %s from the workshop collection...", args[1]);
		ClientPrintAll(HUD_PRINTTALK, CHAT_PREFIX "Changing map to %s...", args[1]);

		new CTimer(5.0f, false, [sCommand]()
		{
			g_pEngineServer2->ServerCommand(sCommand.c_str());
			return -1.0f;
		});

		return;
	}

	// Copy the string, since we're passing this into a timer
	char szMapName[MAX_PATH];
	V_strncpy(szMapName, args[1], sizeof(szMapName));

	ClientPrintAll(HUD_PRINTTALK, CHAT_PREFIX "Changing map to %s...", szMapName);

	new CTimer(5.0f, false, [szMapName]()
	{
		g_pEngineServer2->ChangeLevel(szMapName, nullptr);
		return -1.0f;
	});
}

CON_COMMAND_CHAT_FLAGS(hsay, "<message> - say something as a hud hint", ADMFLAG_CHAT)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !hsay <message>");
		return;
	}

	ClientPrintAll(HUD_PRINTCENTER, "%s", args.ArgS());
}

CON_COMMAND_CHAT_FLAGS(rcon, "<command> - send a command to server console", ADMFLAG_RCON)
{
	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "You are already on the server console.");
		return;
	}

	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !rcon <command>");
		return;
	}

	g_pEngineServer2->ServerCommand(args.ArgS());
}

CON_COMMAND_CHAT_FLAGS(extend, "<minutes> - extend current map (negative value reduces map duration)", ADMFLAG_CHANGEMAP)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !extend <minutes>");
		return;
	}

	int iExtendTime = V_StringToInt32(args[1], 0);

	ConVar* cvar = g_pCVar->GetConVar(g_pCVar->FindConVar("mp_timelimit"));

	// CONVAR_TODO
	// HACK: values is actually the cvar value itself, hence this ugly cast.
	float flTimelimit = *(float *)&cvar->values;

	if (gpGlobals->curtime - g_pGameRules->m_flGameStartTime > flTimelimit * 60)
		flTimelimit = (gpGlobals->curtime - g_pGameRules->m_flGameStartTime) / 60.0f + iExtendTime;
	else
	{
		if (flTimelimit == 1)
			flTimelimit = 0;
		flTimelimit += iExtendTime;
	}

	if (flTimelimit <= 0)
		flTimelimit = 1;

	// CONVAR_TODO
	char buf[32];
	V_snprintf(buf, sizeof(buf), "mp_timelimit %.6f", flTimelimit);
	g_pEngineServer2->ServerCommand(buf);

	const char* pszCommandPlayerName = player ? player->GetPlayerName() : "Console";

	if (iExtendTime < 0)
		ClientPrintAll(HUD_PRINTTALK, CHAT_PREFIX ADMIN_PREFIX "shortened map time %i minutes.", pszCommandPlayerName, iExtendTime * -1);
	else
		ClientPrintAll(HUD_PRINTTALK, CHAT_PREFIX ADMIN_PREFIX "extended map time %i minutes.", pszCommandPlayerName, iExtendTime);
}

void PrecacheAdminBeaconParticle(IEntityResourceManifest* pResourceManifest)
{
	pResourceManifest->AddResource(g_sBeaconParticle.c_str());
}

void KillBeacon(int playerSlot)
{
	ZEPlayer* pPlayer = g_playerManager->GetPlayer(playerSlot);

	if (!pPlayer)
		return;

	CParticleSystem* pParticle = pPlayer->GetBeaconParticle();

	if (!pParticle)
		return;

	pParticle->AcceptInput("DestroyImmediately");

	// delayed Kill because default particle is being silly and remains floating if not Destroyed first
	CHandle<CParticleSystem> hParticle = pParticle->GetHandle();
	new CTimer(0.02f, false, [hParticle]()
	{
		CParticleSystem* particle = hParticle.Get();
		if (particle)
			particle->AcceptInput("Kill");
		return -1.0f;
	});
}

void CreateBeacon(int playerSlot)
{
	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(playerSlot);

	Vector vecAbsOrigin = pTarget->GetPawn()->GetAbsOrigin();

	vecAbsOrigin.z += 10;

	CParticleSystem* particle = (CParticleSystem*)CreateEntityByName("info_particle_system");

	CEntityKeyValues* pKeyValues = new CEntityKeyValues();

	pKeyValues->SetString("effect_name", g_sBeaconParticle.c_str());
	pKeyValues->SetInt("tint_cp", 1);
	pKeyValues->SetVector("origin", vecAbsOrigin);
	// ugly angle change because default particle is rotated
	if (strcmp(g_sBeaconParticle.c_str(), "particles/testsystems/test_cross_product.vpcf") == 0)
		pKeyValues->SetQAngle("angles", QAngle(90, 0, 0));
	
	particle->DispatchSpawn(pKeyValues);
	particle->SetParent(pTarget->GetPawn());

	ZEPlayer* pPlayer = g_playerManager->GetPlayer(playerSlot);
	
	pPlayer->SetBeaconParticle(particle);

	CHandle<CParticleSystem> hParticle = particle->GetHandle();

	// timer persists through map change so serial reset on StartupServer is not needed
	new CTimer(0.0f, true, [playerSlot, hParticle]()
	{
		CCSPlayerController* pPlayer = CCSPlayerController::FromSlot(playerSlot);
		
		if (!pPlayer || pPlayer->m_iTeamNum < CS_TEAM_T || !pPlayer->m_hPlayerPawn->IsAlive())
		{
			KillBeacon(playerSlot);
			return -1.0f;
		}

		CParticleSystem* pParticle = hParticle.Get();

		if (!pParticle)
		{
			return -1.0f;
		}

		// team-based tint of Control Point 1
		if (pPlayer->m_iTeamNum == CS_TEAM_T)
			pParticle->m_clrTint->SetColor(185, 93, 63, 255);
		else
			pParticle->m_clrTint->SetColor(40, 100, 255, 255);
		
		pParticle->AcceptInput("Start");
		// delayed DestroyImmediately input so particle effect can be replayed (and default particle doesn't bug out)
		new CTimer(0.5f, false, [hParticle]()
		{
			CParticleSystem* particle = hParticle.Get();
			if (particle)
				particle->AcceptInput("DestroyImmediately");
			return -1.0f;
		});

		return 1.0f;
	});
}

void PerformBeacon(int playerSlot)
{
	ZEPlayer *pPlayer = g_playerManager->GetPlayer(playerSlot);

	if (!pPlayer->GetBeaconParticle())
		CreateBeacon(playerSlot);
	else
		KillBeacon(playerSlot);
}

CON_COMMAND_CHAT_FLAGS(beacon, "Toggle beacon on a player", ADMFLAG_GENERIC)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !beacon <name>");
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	ETargetType nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlots);

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Target not found.");
		return;
	}

	const char *pszCommandPlayerName = player ? player->GetPlayerName() : "Console";

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		if (!pTarget)
			continue;

		PerformBeacon(pSlots[i]);

		if (nType < ETargetType::ALL)
			PrintSingleAdminAction(pszCommandPlayerName, pTarget->GetPlayerName(), "toggled beacon on");
	}

	PrintMultiAdminAction(nType, pszCommandPlayerName, "toggled beacon on");
}

bool CAdminSystem::LoadAdmins()
{
	m_vecAdmins.Purge();
	KeyValues* pKV = new KeyValues("admins");
	KeyValues::AutoDelete autoDelete(pKV);

	const char *pszPath = "addons/cs2fixes/configs/admins.cfg";

	if (!pKV->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		Warning("Failed to load %s\n", pszPath);
		return false;
	}
	for (KeyValues* pKey = pKV->GetFirstSubKey(); pKey; pKey = pKey->GetNextKey())
	{
		const char *pszName = pKey->GetName();
		const char *pszSteamID = pKey->GetString("steamid", nullptr);
		const char *pszFlags = pKey->GetString("flags", nullptr);

		if (!pszSteamID)
		{
			Warning("Admin entry %s is missing 'steam' key\n", pszName);
			return false;
		}

		if (!pszFlags)
		{
			Warning("Admin entry %s is missing 'flags' key\n", pszName);
			return false;
		}

		ConMsg("Loaded admin %s\n", pszName);
		ConMsg(" - Steam ID %5s\n", pszSteamID);
		ConMsg(" - Flags %5s\n", pszFlags);

		uint64 iFlags = ParseFlags(pszFlags);

		// Let's just use steamID64 for now
		m_vecAdmins.AddToTail(CAdmin(pszName, atoll(pszSteamID), iFlags));
	}

	return true;
}

void CAdminSystem::AddInfraction(CInfractionBase* infraction)
{
	m_vecInfractions.AddToTail(infraction);
}

// This function can run at least twice when a player connects: Immediately upon client connection, and also upon getting authenticated by steam.
// It's also run when we're periodically checking for infraction expiry in the case of mutes/gags.
// This returns false only when called from ClientConnect and the player is banned in order to reject them.
bool CAdminSystem::ApplyInfractions(ZEPlayer *player)
{
	FOR_EACH_VEC(m_vecInfractions, i)
	{
		// Because this can run without the player being authenticated, and the fact that we're applying a ban/mute here,
		// we can immediately just use the steamid we got from the connecting player.
		uint64 iSteamID = player->IsAuthenticated() ? player->GetSteamId64() : player->GetUnauthenticatedSteamId64();

		// We're only interested in infractions concerning this player
		if (m_vecInfractions[i]->GetSteamId64() != iSteamID)
			continue;

		// Undo the infraction just briefly while checking if it ran out
		m_vecInfractions[i]->UndoInfraction(player);

		time_t timestamp = m_vecInfractions[i]->GetTimestamp();
		if (timestamp != 0 && timestamp <= std::time(0))
		{
			m_vecInfractions.Remove(i);
			continue;
		}

		// We are called from ClientConnect and the player is banned, immediately reject them
		if (!player->IsConnected() && m_vecInfractions[i]->GetType() == CInfractionBase::EInfractionType::Ban)
			return false;

		m_vecInfractions[i]->ApplyInfraction(player);
	}
	return true;
}

bool CAdminSystem::FindAndRemoveInfraction(ZEPlayer *player, CInfractionBase::EInfractionType type, bool bRemoveSession)
{
	if (!player || player->IsFakeClient() || !player->IsAuthenticated())
		return false;
	bool bRemovedPunishment = false;

	FOR_EACH_VEC_BACK(m_vecInfractions, i)
	{
		if (m_vecInfractions[i]->GetSteamId64() == player->GetSteamId64() && m_vecInfractions[i]->GetType() == type &&
			(bRemoveSession || !m_vecInfractions[i]->IsSession()))
		{
			m_vecInfractions[i]->UndoInfraction(player);
			m_vecInfractions.Remove(i);

			bRemovedPunishment = true;
		}
	}

	if (bRemovedPunishment && !bRemoveSession)
	{
	// If we undid a timed block but haven't touched session blocks, make sure any session blocks still apply
		FOR_EACH_VEC(m_vecInfractions, i)
		{
			if (m_vecInfractions[i]->GetSteamId64() == player->GetSteamId64() && m_vecInfractions[i]->GetType() == type)
				m_vecInfractions[i]->ApplyInfraction(player);
		}
	}
	return bRemovedPunishment;
}

CAdmin *CAdminSystem::FindAdmin(uint64 iSteamID)
{
	FOR_EACH_VEC(m_vecAdmins, i)
	{
		if (m_vecAdmins[i].GetSteamID() == iSteamID)
			return &m_vecAdmins[i];
	}

	return nullptr;
}

uint64 CAdminSystem::ParseFlags(const char* pszFlags)
{
	uint64 flags = 0;
	size_t length = V_strlen(pszFlags);

	for (size_t i = 0; i < length; i++)
	{
		char c = tolower(pszFlags[i]);
		if (c < 'a' || c > 'z')
			continue;

		if (c == 'z')
			return -1; // all flags

		flags |= ((uint64)1 << (c - 'a'));
	}

	return flags;
}

void CBanInfraction::ApplyInfraction(ZEPlayer *player)
{
	g_pEngineServer2->DisconnectClient(player->GetPlayerSlot(), NETWORK_DISCONNECT_KICKBANADDED); // "Kicked and banned"
}

void CMuteInfraction::ApplyInfraction(ZEPlayer* player)
{
	player->SetMuted(true);
}

void CMuteInfraction::UndoInfraction(ZEPlayer *player)
{
	player->SetMuted(false);
}

void CGagInfraction::ApplyInfraction(ZEPlayer *player)
{
	player->SetGagged(true);
}

void CGagInfraction::UndoInfraction(ZEPlayer *player)
{
	player->SetGagged(false);
}

//--------------------------------------------------------------------------------------------------
// GFLBans Specific stuff (kept mostly seperate for easier merging with CS2Fixes public repo)
//--------------------------------------------------------------------------------------------------

static std::string g_strGFLBansApiUrl = "https://bans.aurora.vg/api/v1/";
static bool g_bGFLBansAllServers = true;
static int g_iMinOfflineDurations = 61;
static std::string g_strGFLBansHostname = "CS2 ZE Test";
static std::string g_strGFLBansServerID = "999";
static std::string g_strGFLBansServerKey = "1337";
static std::vector<HTTPHeader>* g_rghdGFLBansAuth = new std::vector<HTTPHeader>{HTTPHeader("Authorization", "SERVER " + g_strGFLBansServerID + " " + g_strGFLBansServerKey)};
// This only affects the CURRENT report being sent and is not logged in GFLBans. This means that
// if a report was sent 1 minute ago with a cooldown of 600 seconds, but a new report is sent with a
// 20 second cooldown, the new report will be successfull. So for an emergency report that you need
// to force through, set this to 1, send the report, then change it back to the original value
static int g_iGFLBansReportCooldown = 600; 

FAKE_STRING_CVAR(gflbans_api_url, "URL to interact with GFLBans API. Should end in \"api/v1/\"", g_strGFLBansApiUrl, false)
FAKE_STRING_CVAR(gflbans_hostname, "Name of the server", g_strGFLBansHostname, false) // remove once we can read hostname convar
FAKE_BOOL_CVAR(gflbans_include_other_servers, "Enables checking punishments from other GFL servers", g_bGFLBansAllServers, true, false)
FAKE_INT_CVAR(gflbans_min_offline_punish_duration, "Minimum amount of minutes for a mute/gag duration to tick down while client is disconnected. 0 or negative values force all punishments Online Only", g_iMinOfflineDurations, 61, false)
FAKE_INT_CVAR(gflbans_report_cooldown, "Minimum amount of seconds between !report/!calladmin usages. Minimum of 1 second.", g_iGFLBansReportCooldown, 600, false)

//These need to update g_rghdGFLBansAuth when changed, but otherwise work just like a fake cvar as if they weren't commented out:
// FAKE_STRING_CVAR(gflbans_server_id, "GFLBans ID for the server. DO NOT LEAK THIS", g_strGFLBansServerID, true)
CON_COMMAND_F(gflbans_server_id, "GFLBans ID for the server. DO NOT LEAK THIS", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY | FCVAR_PROTECTED)
{
	if (args.ArgC() < 2)
	{
		Msg("%s %s\n", args[0], g_strGFLBansServerID.c_str());
		return;
	}

	g_strGFLBansServerID = args[1];
	g_rghdGFLBansAuth->clear();
	g_rghdGFLBansAuth->push_back(HTTPHeader("Authorization", "SERVER " + g_strGFLBansServerID + " " + g_strGFLBansServerKey));
}

// FAKE_STRING_CVAR(gflbans_server_key, "GFLBans KEY for the server. DO NOT LEAK THIS", g_strGFLBansServerKey, true)
CON_COMMAND_F(gflbans_server_key, "GFLBans KEY for the server. DO NOT LEAK THIS", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY | FCVAR_PROTECTED)
{
	if (args.ArgC() < 2)
	{
		Msg("%s %s\n", args[0], g_strGFLBansServerKey.c_str());
		return;
	}

	g_strGFLBansServerKey = args[1];
	g_rghdGFLBansAuth->clear();
	g_rghdGFLBansAuth->push_back(HTTPHeader("Authorization", "SERVER " + g_strGFLBansServerID + " " + g_strGFLBansServerKey));
}

CON_COMMAND_CHAT(report, "<name> <reason> - report a player")
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !report <name> <reason>");
		return;
	}

	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "A calling player is required by GFLBans, so you may not report through console. Use \"c_info <name>\" to find their information instead.");
		return;
	}

	int iCommandPlayer = player->GetPlayerSlot();
	int iNumClients = 0;
	int pSlot[MAXPLAYERS];

	if (g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlot) != ETargetType::PLAYER || iNumClients > 1)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"You can only report individual players.");
		return;
	}

	if (!iNumClients)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Player not found.");
		return;
	}

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[0]);

	if (!pTarget)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Target not found.");
		return;
	}

	ZEPlayer* pTargetPlayer = g_playerManager->GetPlayer(pSlot[0]);

	if (pTargetPlayer->IsFakeClient())
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"You may not report bots, consider using !calladmin instead.");
		return;
	}

	if (player->GetZEPlayer() == pTargetPlayer)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"You may not report yourself, consider using !calladmin instead.");
		return;
	}

	if (!pTargetPlayer->IsAuthenticated())
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is not yet authenticated with Steam, please wait a bit and then try again or use !calladmin instead.", pTarget->GetPlayerName());
		return;
	}

	std::shared_ptr<GFLBans_PlayerObjNoIp> plyBadPerson = std::make_shared<GFLBans_PlayerObjNoIp>(pTargetPlayer);
	std::shared_ptr<GFLBans_PlayerObjNoIp> plyCaller = std::make_shared<GFLBans_PlayerObjNoIp>(player->GetZEPlayer());
	std::string strMessage = "";
	for (int i = 2; i < args.ArgC(); i++)
		strMessage = strMessage + args[i] + " ";
	if (strMessage.length() > 0)
		strMessage = strMessage.substr(0, strMessage.length() - 1);
	else
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"You must provide a reason for reporting.");
		return;
	}

	GFLBans_Report report(plyCaller, player->GetPlayerName(), strMessage, plyBadPerson, pTarget->GetPlayerName());
	report.GFLBans_CallAdmin(player);
}

CON_COMMAND_CHAT(calladmin, "<reason> - request for an admin to join the server")
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !calladmin <reason>");
		return;
	}

	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "A calling player is required by GFLBans, so you may not call admins through console.");
		return;
	}

	std::shared_ptr<GFLBans_PlayerObjNoIp> plyCaller = std::make_shared<GFLBans_PlayerObjNoIp>(player->GetZEPlayer());
	std::string strMessage = "";
	for (int i = 1; i < args.ArgC(); i++)
		strMessage = strMessage + args[i] + " ";
	if (strMessage.length() > 0)
		strMessage = strMessage.substr(0, strMessage.length() - 1);
	else
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"You must provide a reason for calling an admin.");
		return;
	}

	GFLBans_Report report(plyCaller, player->GetPlayerName(), strMessage);
	report.GFLBans_CallAdmin(player);
}

CON_COMMAND_CHAT_FLAGS(claim, "- claims the most recent GFLBans report/calladmin query", ADMFLAG_KICK)
{
	json jClaim;
	jClaim["admin_name"] = player ? player->GetPlayerName() : "CONSOLE";

	if (std::time(nullptr) - g_pAdminSystem->m_wLastHeartbeat > 120)
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "GFLBans is currently not responding. Your claim may fail.");

#ifdef _DEBUG
	Message(("Claim Query:\n" + g_strGFLBansApiUrl + "gs/calladmin/claim\n").c_str());
	if (g_rghdGFLBansAuth != nullptr)
	{
		for (HTTPHeader header : *(g_rghdGFLBansAuth))
		{
			Message("Header - %s: %s\n", header.GetName(), header.GetValue());
		}
	}
	Message((jClaim.dump(1) + "\n").c_str());
#endif
	g_HTTPManager.POST((g_strGFLBansApiUrl + "gs/calladmin/claim").c_str(),
					   jClaim.dump().c_str(),
					   [player](HTTPRequestHandle request, json response) {
	#ifdef _DEBUG
		Message(("Claim Response:\n" + response.dump(1) + "\n").c_str());
	#endif

		if (!response.value("success", false))
		{
			if (response.value("msg", "").length() > 0)
				ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s", response.value("msg", "").c_str());
			else
				ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Claim request failed. Are you sure there is an open admin call?");
		}
		else
		{
			if (response.value("msg", "").length() > 0)
				ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s", response.value("msg", "").c_str());
			else
				ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Successfully claimed an admin call.");
		}

	}, g_rghdGFLBansAuth);
}

CON_COMMAND_CHAT(status, "<name> - List a player's active punishments. Non-admins may only check their own punishments")
{
	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients;
	int pSlot[MAXPLAYERS];
	ETargetType nType = ETargetType::PLAYER;
	ZEPlayer* pTargetPlayer = nullptr;
	bool bIsAdmin = g_playerManager->GetPlayer(iCommandPlayer)->IsAdminFlagSet(ADMFLAG_CHAT | ADMFLAG_BAN);
	std::string target = !bIsAdmin || args.ArgC() == 1 ? "" : args[1];

	if (bIsAdmin && target.length() > 0)
	{
		iNumClients = 0;
		target = args[1];
		nType = g_playerManager->TargetPlayerString(iCommandPlayer, args[1], iNumClients, pSlot);
	}
	else
	{
		iNumClients = 1;
		pSlot[0] = iCommandPlayer;
	}

	if (iNumClients > 1)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"You can only target individual players for listing punishments.");
		return;
	}
	else if (iNumClients <= 0)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Target not found.");
		return;
	}

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[0]);
	if (!pTarget)
		return;

	pTargetPlayer = g_playerManager->GetPlayer(pSlot[0]);

	if (pTargetPlayer->IsFakeClient())
		return;

	if (!pTargetPlayer->IsMuted() && !pTargetPlayer->IsGagged())
	{
		if (target.length() == 0)
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"You have no active punishments.");
		else
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"%s has no active punishments.", pTarget->GetPlayerName());
		return;
	}

	std::shared_ptr<GFLBans_PlayerObjIPOptional> gflPlayer = std::make_shared<GFLBans_PlayerObjIPOptional>(pTargetPlayer);

	if (gflPlayer->m_strGSID == "BOT")
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Bots cannot have punishments.");
		return;
	}

	// Send the requests
	std::string strURL = gflPlayer->GB_Query();
#ifdef _DEBUG
	Message(("Request URL: " + strURL + "\n").c_str());
#endif
	if (strURL.length() == 0)
		return;
	
	g_HTTPManager.GET(strURL.c_str(), [player, target, pTargetPlayer](HTTPRequestHandle request, json response)
	{
#ifdef _DEBUG
		Message("status response: %s\n", response.dump().c_str());
#endif
		std::string punishment = "";
		if (pTargetPlayer->IsMuted() && pTargetPlayer->IsGagged())
			punishment = "\2gagged\1 and \2muted\1";
		else if (pTargetPlayer->IsMuted())
			punishment = "\2muted\1";
		else if (pTargetPlayer->IsGagged())
			punishment = "\2gagged\1";

		if (response.dump().length() < 5)
		{
			if (punishment.length() > 0)
				ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"%s %s.",
							target.length() == 0 ? "You are" : (target + " is").c_str(),
							punishment.c_str());
			else
				ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"%s no active punishments.", 
							target.length() == 0 ? "You have" : (target + " has").c_str());
			
			return;
		}
		
		if (pTargetPlayer == nullptr)
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Check console for punishment information.");
		else
		{
			if (pTargetPlayer->IsMuted() && pTargetPlayer->IsGagged())
				punishment = "\2gagged\1 and \2muted\1";
			else if (pTargetPlayer->IsMuted())
				punishment = "\2muted\1";
			else if (pTargetPlayer->IsGagged())
				punishment = "\2gagged\1";

			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"%s currently %s. Check console for more information.",
						target.length() == 0 ? "You are" : (target + " is").c_str(),
						punishment.c_str());
		}

		if (target.length() == 0)
			ClientPrint(player, HUD_PRINTCONSOLE, "[GFLBans] Your active punishments:");
		else
			ClientPrint(player, HUD_PRINTCONSOLE, "[GFLBans] Active punishments for %s:", (target).c_str());

		ConsoleListPunishments(player, response);
	}, g_rghdGFLBansAuth);
}

CON_COMMAND_CHAT_FLAGS(listdc, "- List recently disconnected players and their Steam64 IDs", ADMFLAG_CHAT | ADMFLAG_BAN)
{
	g_pAdminSystem->ShowDisconnectedPlayers(player);
}

#ifdef _DEBUG
CON_COMMAND_CHAT_FLAGS(add_dc, "<name> <SteamID 64> <IP Address> - Adds a fake player to disconnected player list for testing", ADMFLAG_CHAT | ADMFLAG_BAN)
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !add_dc <name> <Steam64 ID> <IP Address>");
		return;
	}

	std::string strSteamID = args[2];
	if (strSteamID.length() != 17 || std::find_if(strSteamID.begin(), strSteamID.end(), [](unsigned char c) { return !std::isdigit(c); }) != strSteamID.end())
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX"Invalid Steam64 ID.");
		return;
	}
	// stoll should be exception safe with above check
	uint64 iSteamID = std::stoll(strSteamID);

	g_pAdminSystem->AddDisconnectedPlayer(args[1], iSteamID, args[3]);
}

CON_COMMAND_CHAT_FLAGS(dumpinf, "- Dump server's infractions table", ADMFLAG_CHAT | ADMFLAG_BAN)
{
	g_pAdminSystem->DumpInfractions();
}

void CAdminSystem::DumpInfractions()
{
	FOR_EACH_VEC(m_vecInfractions, i)
	{
		Message(("Infraction: " + std::to_string(m_vecInfractions[i]->GetType()) +
				"\n\tSteamID:" + std::to_string(m_vecInfractions[i]->GetSteamId64()) +
				"\n\tSession: " + std::to_string(m_vecInfractions[i]->IsSession()) + "\n").c_str());
	}
}
#endif

GFLBans_PlayerObjNoIp::GFLBans_PlayerObjNoIp(ZEPlayer* player)
{
	if (!player || player->IsFakeClient())
		m_strGSID = "BOT";
	else
		m_strGSID = std::to_string(player->IsAuthenticated() ? player->GetSteamId64() : player->GetUnauthenticatedSteamId64());
}

json GFLBans_PlayerObjNoIp::CreateInfractionJSON() const
{
	json jRequestBody;
	jRequestBody["gs_service"] = "steam";
	jRequestBody["gs_id"] = m_strGSID;
	return jRequestBody;
}

inline std::string GFLBans_PlayerObjNoIp::GB_Query() const
{
	if (m_strGSID == "Bot")
		return "";
	else if (g_bGFLBansAllServers)
		return g_strGFLBansApiUrl + "infractions/check?gs_service=steam&gs_id=" + m_strGSID;
	else
		return g_strGFLBansApiUrl + "infractions/check?gs_service=steam&gs_id=" + m_strGSID +
		"&include_other_servers=false";
}

GFLBans_PlayerObjIPOptional::GFLBans_PlayerObjIPOptional(ZEPlayer* player) :
	GFLBans_PlayerObjNoIp(player)
{
	if (!player || player->IsFakeClient())
		m_strIP = "";
	else
	{
		m_strIP = player->GetIpAddress();
		if (!HasIP())
			m_strIP = "";
	}
}

json GFLBans_PlayerObjIPOptional::CreateInfractionJSON() const
{
	json jRequestBody = GFLBans_PlayerObjNoIp::CreateInfractionJSON();
	if (HasIP())
		jRequestBody["ip"] = m_strIP;
	return jRequestBody;
}

inline bool GFLBans_PlayerObjIPOptional::HasIP() const
{
	//TODO: Should check and treat local IPs as invalid (more than just invalidating "loopback").
	//      Should do actual ip parsing too. ie <0-255>.<0-255>.<0-255>.<0-255>
	return m_strIP.length() > 6 && m_strIP != "127.0.0.1" && std::find_if(m_strIP.begin(), m_strIP.end(), [](unsigned char c) { return !std::isdigit(c) && c != '.'; }) == m_strIP.end();
}

inline std::string GFLBans_PlayerObjIPOptional::GB_Query() const
{
	std::string strURL = GFLBans_PlayerObjNoIp::GB_Query();

	if (!HasIP())
		return strURL;

	if (strURL.length() == 0)
		strURL = g_strGFLBansApiUrl + "infractions/check?ip=";
	else
		strURL.append("&ip=");

	strURL.append(m_strIP);
	if (!g_bGFLBansAllServers)
		strURL.append("&include_other_servers=false");

	return strURL;
}

GFLBans_InfractionBase::GFLBans_InfractionBase(std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson,
											   GFLInfractionType gitType, std::string strReason,
											   std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin) :
	m_plyBadPerson(plyBadPerson), m_wAdmin(plyAdmin), m_gitType(gitType)
{
	m_strReason = strReason.length() == 0 ? "No reason provided" :
		strReason.length() <= 280 ? strReason : strReason.substr(0, 280);
}

inline auto GFLBans_InfractionBase::GetInfractionType() const noexcept -> GFLInfractionType
{
	return m_gitType;
}

GFLBans_Infraction::GFLBans_Infraction(std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson,
									   GFLInfractionType gitType, std::string strReason,
									   std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin,
									   int iDuration, bool bOnlineOnly) :
	GFLBans_InfractionBase(plyBadPerson, gitType, strReason, plyAdmin),
	m_bOnlineOnly(bOnlineOnly)
{
	m_wCreated = std::time(nullptr);
	m_wExpires = m_wCreated + (iDuration * 60);

	m_gisScope = g_bGFLBansAllServers ? Global : Server;
}

inline bool GFLBans_Infraction::IsSession() const noexcept
{
	return m_wExpires < m_wCreated && m_gitType != Ban;
}

json GFLBans_Infraction::CreateInfractionJSON() const
{
	json jRequestBody;
	int iDuration = m_wExpires - m_wCreated;

	// Omit duration for perma
	if (iDuration > 0)
		jRequestBody["duration"] = iDuration;
	else if (IsSession())
		jRequestBody["session"] = true;

	jRequestBody["player"] = m_plyBadPerson->CreateInfractionJSON();

	// Omit admin for block through Console
	if (m_wAdmin != nullptr)
	{
		json jAdmin;
		jAdmin["gs_admin"] = m_wAdmin->CreateInfractionJSON();
		jRequestBody["admin"] = jAdmin;
	}

	jRequestBody["reason"] = m_strReason;

	json jPunishments;
	switch (m_gitType)
	{
		case Mute:
			jPunishments[0] = "voice_block";
			break;
		case Gag:
			jPunishments[0] = "chat_block";
			break;
		case Ban:
			jPunishments[0] = "ban";
			break;
		case AdminChatGag:
			jPunishments[0] = "admin_chat_block";
			break;
		case CallAdminBlock:
			jPunishments[0] = "call_admin_block";
			break;
		case Silence:
			jPunishments[0] = "voice_block";
			jPunishments[1] = "chat_block";
			break;
	}
	jRequestBody["punishments"] = jPunishments;

	switch (m_gisScope)
	{
		case Server:
			jRequestBody["scope"] = "server";
			break;
		case Global:
			jRequestBody["scope"] = "global";
			break;
	}

	if (m_bOnlineOnly && m_gitType != Ban && iDuration > 0)
		jRequestBody["dec_online_only"] = true;

	return jRequestBody;
}

json GFLBans_RemoveInfractionsOfPlayer::CreateInfractionJSON() const
{
	json jRequestBody;

	jRequestBody["player"] = m_plyBadPerson->CreateInfractionJSON();
	jRequestBody["remove_reason"] = m_strReason;

	// Omit admin for block through Console
	if (m_wAdmin != nullptr)
	{
		json jAdmin;
		jAdmin["gs_admin"] = m_wAdmin->CreateInfractionJSON();
		jRequestBody["admin"] = jAdmin;
	}

	//Omit for default true
	if (!g_bGFLBansAllServers)
		jRequestBody["include_other_servers"] = g_bGFLBansAllServers;

	json jPunishments;
	switch (m_gitType)
	{
		case Mute:
			jPunishments[0] = "voice_block";
			break;
		case Gag:
			jPunishments[0] = "chat_block";
			break;
		case Ban:
			jPunishments[0] = "ban";
			break;
		case AdminChatGag:
			jPunishments[0] = "admin_chat_block";
			break;
		case CallAdminBlock:
			jPunishments[0] = "call_admin_block";
			break;
		case Silence:
			jPunishments[0] = "voice_block";
			jPunishments[1] = "chat_block";
			break;
	}
	jRequestBody["restrict_types"] = jPunishments;

	return jRequestBody;
}

GFLBans_Report::GFLBans_Report(std::shared_ptr<GFLBans_PlayerObjNoIp> plyCaller, std::string strCallerName,
							   std::string strMessage, std::shared_ptr<GFLBans_PlayerObjNoIp> plyBadPerson,
							   std::string strBadPersonName) :
	m_plyCaller(plyCaller), m_strCallerName(strCallerName),
	m_plyBadPerson(plyBadPerson), m_strBadPersonName(strBadPersonName)
{
	m_strMessage = strMessage.length() == 0 ? "No reason provided" :
		strMessage.length() <= 120 ? strMessage : strMessage.substr(0, 120);
}

json GFLBans_Report::CreateReportJSON() const
{
	json jRequestBody;

	jRequestBody["caller"] = m_plyCaller->CreateInfractionJSON();
	jRequestBody["caller_name"] = m_strCallerName;

	// Omit for false
	if (g_bGFLBansAllServers)
		jRequestBody["include_other_servers"] = true;

	jRequestBody["message"] = m_strMessage;

	// Omit for 10 minutes
	if (g_iGFLBansReportCooldown != 600 && g_iGFLBansReportCooldown >= 0)
		jRequestBody["cooldown"] = g_iGFLBansReportCooldown;

	// Omit for generic call admin (no target of report)
	if (m_plyBadPerson != nullptr)
	{
		jRequestBody["report_target"] = m_plyBadPerson->CreateInfractionJSON();
		jRequestBody["report_target_name"] = m_strBadPersonName;
	}

	return jRequestBody;
}

inline bool GFLBans_Report::IsReport() const noexcept
{
	return m_plyBadPerson != nullptr;
}

void GFLBans_Report::GFLBans_CallAdmin(CCSPlayerController* pCaller)
{
	if (!pCaller)
		return;

	// Pass this into callback function, so we know if the response is for a report or calladmin query
	bool bIsReport = IsReport();

	if (std::time(nullptr) - g_pAdminSystem->m_wLastHeartbeat > 120)
		ClientPrint(pCaller, HUD_PRINTTALK, CHAT_PREFIX "GFLBans is currently not responding. Your %s may fail.",
					bIsReport ? "report"  : "calladmin request");

#ifdef _DEBUG
	Message(("Report/CallAdmin Query:\n" + g_strGFLBansApiUrl + "gs/calladmin/\n").c_str());
	if (g_rghdGFLBansAuth != nullptr)
	{
		for (HTTPHeader header : *(g_rghdGFLBansAuth))
		{
			Message("Header - %s: %s\n", header.GetName(), header.GetValue());
		}
	}
	Message((CreateReportJSON().dump(1) + "\n").c_str());
#endif
	g_HTTPManager.POST((g_strGFLBansApiUrl + "gs/calladmin/").c_str(),
					   CreateReportJSON().dump().c_str(),
					   [pCaller, bIsReport](HTTPRequestHandle request, json response) {
	#ifdef _DEBUG
		Message(("Report/CallAdmin Response:\n" + response.dump(1) + "\n").c_str());
	#endif

		if (!response.value("sent", false))
		{
			if (response.value("is_banned", true))
				ClientPrint(pCaller, HUD_PRINTTALK, CHAT_PREFIX "You are banned from using !report and !calladmin. Please check GFLBans for more information.");
			else if (response.value("cooldown", 0) > 0)
				ClientPrint(pCaller, HUD_PRINTTALK, CHAT_PREFIX "The %s command was used recently and is on cooldown for \2%s\1.",
							bIsReport ? "report" : "calladmin", FormatTime(response.value("cooldown", 0)).c_str());
			else
				ClientPrint(pCaller, HUD_PRINTTALK, CHAT_PREFIX "Your %s failed to send. Please try again",
							bIsReport ? "report" : "admin call");
		}
		else
		{
			ClientPrint(pCaller, HUD_PRINTTALK, CHAT_PREFIX "Your %s was sent. If an admin is available, they will help out as soon as possible.",
						bIsReport ? "report" : "admin request");
		}
		
	}, g_rghdGFLBansAuth);
}


CAdminSystem::CAdminSystem()
{
	LoadAdmins();

	// Make sure other functions know no heartbeat yet (they check if it was at most 120 seconds ago)
	m_wLastHeartbeat = std::time(nullptr) - 121;

	// Fill out disconnected player list with empty objects which we overwrite as players leave
	for (int i = 0; i < 20; i++)
		m_rgDCPly[i] = std::tuple<std::string, uint64, std::string>("", 0, "");
	m_iDCPlyIndex = 0;
}

void CAdminSystem::RemoveAllPunishments()
{
	m_vecInfractions.PurgeAndDeleteElements();

	if (!gpGlobals || !g_playerManager)
		return;

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		ZEPlayer* pPlayer = g_playerManager->GetPlayer(i);

		if (!pPlayer || pPlayer->IsFakeClient())
			continue;

		pPlayer->CheckInfractions();
	}
}

void CAdminSystem::RemoveSessionPunishments(float fDelay)
{
	if (fDelay > 0)
	{
		new CTimer(fDelay, true, []() {
			g_pAdminSystem->RemoveSessionPunishments(-1);
			return -1.0f;
		});
		return;
	}

#ifdef _DEBUG
	Message("Attempting to remove all session punishments\n");
#endif
	FOR_EACH_VEC_BACK(m_vecInfractions, i)
	{
		time_t timestamp = m_vecInfractions[i]->GetTimestamp();
		if (!m_vecInfractions[i]->IsSession() && (timestamp > std::time(nullptr) ||
												 (timestamp == 0 && fDelay == -1)))
		{
		// Dont remove map session blocks (timestamp == 0) if this is called
		// due to remove group targetting blocks like !mute @t 1 (negative fDelay)
			continue;
		}

		ZEPlayer* pPlayer = g_playerManager->GetPlayerFromSteamId(m_vecInfractions[i]->GetSteamId64());

		if (!pPlayer || pPlayer->IsFakeClient())
			continue;

		m_vecInfractions[i]->UndoInfraction(pPlayer);
		m_vecInfractions.Remove(i);
	}
}


// https://github.com/GFLClan/sm_gflbans/wiki/Implementer's-Guide-to-the-GFLBans-API#heartbeat
bool CAdminSystem::GFLBans_Heartbeat()
{
	if (!gpGlobals || gpGlobals->maxClients < 2)
		return false;

#ifdef _DEBUG
	Message("Heartbeat!\n");
#endif

	json jHeartbeat;

	// TODO: Properly implement when MM can read a convar's string value
	//jHeartbeat["hostname"] = g_pCVar->GetConVar(g_pCVar->FindConVar("hostname"))->GetString();
	jHeartbeat["hostname"] = g_strGFLBansHostname;

	jHeartbeat["max_slots"] = gpGlobals->maxClients;


	json jPlayers = json::array();
	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		ZEPlayer* pPlayer = g_playerManager->GetPlayer(i);

		if (!pPlayer || pPlayer->IsFakeClient())
			continue;

		GFLBans_PlayerObjIPOptional gflPlayer(pPlayer);

		jPlayers.push_back(gflPlayer.CreateInfractionJSON());
	}
	jHeartbeat["players"] = jPlayers;
#ifdef _WIN32
	jHeartbeat["operating_system"] = "windows";
#else
	jHeartbeat["operating_system"] = "linux";
#endif
	jHeartbeat["mod"] = "cs2"; // Should this be "cs2" or "csgo"?
	jHeartbeat["map"] = gpGlobals->mapname.ToCStr();

	// Omit for false.
	// TODO: Properly implement when MM can read a convar's string value
	//if (g_pCVar->GetConVar(g_pCVar->FindConVar("sv_password"))->GetString().length() > 0)
	//	jHeartbeat["locked"] = true;

	// Omit for true
	if (!g_bGFLBansAllServers)
		jHeartbeat["include_other_servers"] = false;

#ifdef _DEBUG
	if (g_rghdGFLBansAuth != nullptr)
	{
		for (HTTPHeader header : *g_rghdGFLBansAuth)
		{
			Message("Heartbeat Header - %s: %s\n", header.GetName(), header.GetValue());
		}
	}
	Message(("Heartbeat Query:\nURL: " + g_strGFLBansApiUrl +
			 "gs/heartbeat\nPOST JSON:\n" + jHeartbeat.dump(1) + "\n").c_str());
#endif

	g_HTTPManager.POST((g_strGFLBansApiUrl + "gs/heartbeat").c_str(), jHeartbeat.dump().c_str(),
					   [](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
		Message(("Heartbeat Response:\n" + response.dump(1) + "\n").c_str());
#endif
		g_pAdminSystem->m_wLastHeartbeat = std::time(nullptr);
		for (auto& [key, heartbeatChange] : response.items())
		{
			json jInfractions = heartbeatChange.value("check", json());
			json jPly = heartbeatChange.value("player", json());
			if (jPly.empty() || jPly.value("gs_service", "") != "steam")
				continue;

			std::string strSteamID = jPly.value("gs_id", "");
			if (strSteamID.length() != 17 || std::find_if(strSteamID.begin(), strSteamID.end(), [](unsigned char c) { return !std::isdigit(c); }) != strSteamID.end())
				continue;
			// stoll should be exception safe with above check
			uint64 iSteamID = std::stoll(strSteamID);

			ZEPlayer* pPlayer = g_playerManager->GetPlayerFromSteamId(iSteamID, true);

			if (!pPlayer || pPlayer->IsFakeClient())
				continue;

			CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pPlayer->GetPlayerSlot());

			bool bWasPunished = pPlayer->IsMuted();
			if (!g_pAdminSystem->CheckJSONForBlock(pPlayer, jInfractions, GFLBans_InfractionBase::GFLInfractionType::Mute, true, false)
				&& bWasPunished && !pPlayer->IsMuted())
				ClientPrint(pTarget, HUD_PRINTTALK, CHAT_PREFIX "You are no longer muted. You may talk again.");

			bWasPunished = pPlayer->IsGagged();
			if (!g_pAdminSystem->CheckJSONForBlock(pPlayer, jInfractions, GFLBans_InfractionBase::GFLInfractionType::Gag, true, false)
				&& bWasPunished && !pPlayer->IsGagged())
				ClientPrint(pTarget, HUD_PRINTTALK, CHAT_PREFIX "You are no longer gagged. You may type in chat again.");

			//g_pAdminSystem->CheckJSONForBlock(pPlayer, jInfractions, GFLBans_InfractionBase::GFLInfractionType::AdminChatGag, true, false);
			// We dont need to check to apply a Call Admin Block server side, since that is all handled by GFLBans itself
			g_pAdminSystem->CheckJSONForBlock(pPlayer, jInfractions, GFLBans_InfractionBase::GFLInfractionType::Ban);
		}
	}, g_rghdGFLBansAuth);
	return true;
}

// https://github.com/GFLClan/sm_gflbans/wiki/Implementer's-Guide-to-the-GFLBans-API#checking-player-infractions
void CAdminSystem::GFLBans_CheckPlayerInfractions(ZEPlayer* player)
{
	if (player == nullptr || player->IsFakeClient())
		return;

	// Check against current infractions on the server and remove expired ones
	ApplyInfractions(player);

	// We dont care if player is authenticated or not at this point.
	// We are just fetching active punishments rather than applying anything new, so innocents
	// cannot be hurt by someone faking a steamid here
	std::string iSteamID = std::to_string(player->IsAuthenticated() ? player->GetSteamId64() : player->GetUnauthenticatedSteamId64());

	GFLBans_PlayerObjIPOptional gflPlayer(player);

	std::string strURL = gflPlayer.GB_Query();
#ifdef _DEBUG
	Message((strURL + "\n").c_str());
#endif
	if (strURL.length() == 0)
		return;

	g_HTTPManager.GET(strURL.c_str(), [player](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
		Message(("Check Infraction Response:\n" + response.dump(1) + "\n").c_str());
#endif
		g_pAdminSystem->CheckJSONForBlock(player, response, GFLBans_InfractionBase::GFLInfractionType::Mute, true, false);
		g_pAdminSystem->CheckJSONForBlock(player, response, GFLBans_InfractionBase::GFLInfractionType::Gag, true, false);
		g_pAdminSystem->CheckJSONForBlock(player, response, GFLBans_InfractionBase::GFLInfractionType::Ban, true, false);
		//g_pAdminSystem->CheckJSONForBlock(player, response, GFLBans_InfractionBase::GFLInfractionType::AdminChatGag, true, false);
		// We dont need to check to apply a Call Admin Block server side, since that is all handled by GFLBans itself
	}, g_rghdGFLBansAuth);
}

// https://github.com/GFLClan/sm_gflbans/wiki/Implementer's-Guide-to-the-GFLBans-API#old-style-infractions
void CAdminSystem::GFLBans_CreateInfraction(std::shared_ptr<GFLBans_Infraction> infPunishment,
											ZEPlayer* plyBadPerson, CCSPlayerController* pAdmin)
{
	if (!plyBadPerson || plyBadPerson->IsFakeClient() || !plyBadPerson->IsAuthenticated())
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "The player is not on the server...");
		return;
	}

	if ((infPunishment->IsSession() && infPunishment->GetInfractionType() != GFLBans_InfractionBase::GFLInfractionType::Ban) ||
		std::time(nullptr) - m_wLastHeartbeat > 120)
	{
	// Make sure session punishments are applied. It would be nice to log them, but if GFLBans
	// is down, we still want these to stick. Also, if last 2 heartbeats failed, assume
	// GFLBans is down and simply apply a map-length punishment as well to make sure it sticks
	// until GFLBans is back up.
		CInfractionBase* infraction;
		std::string strPunishment = "";
		switch (infPunishment->GetInfractionType())
		{
			case GFLBans_InfractionBase::GFLInfractionType::Mute:
				strPunishment = "muted";
				infraction = new CMuteInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Gag:
				strPunishment = "gagged";
				infraction = new CGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Ban:
				strPunishment = "banned";
				infraction = new CBanInfraction(0, plyBadPerson->GetSteamId64());
				break;
			default:
				// This should never be reached, since we it means we are trying to apply an unimplemented block type
				ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "Improper block type... Send to a dev with the command used.");
				return;
		}

		std::string strBadPlyName = CCSPlayerController::FromSlot(plyBadPerson->GetPlayerSlot())->GetPlayerName();

		strPunishment.append(" " + strBadPlyName + " until the map changes");

		if (infPunishment->GetReason() != "No reason provided")
			strPunishment.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");

		if (std::time(nullptr) - m_wLastHeartbeat > 120)
			strPunishment.append(" (\2GFLBans is currently not responding\1)");

		const char* pszAdminName = pAdmin ? pAdmin->GetPlayerName() : "Console";
		ClientPrintAll(HUD_PRINTTALK, CHAT_PREFIX ADMIN_PREFIX "%s%s%s.", pszAdminName, "", strPunishment.c_str(), "");

		// We're overwriting the infraction, so remove the previous one first
		g_pAdminSystem->AddInfraction(infraction);
		infraction->ApplyInfraction(plyBadPerson);
	}

#ifdef _DEBUG
	Message(("Create Infraction Query:\n" + g_strGFLBansApiUrl + "infractions/\n").c_str());
	if (g_rghdGFLBansAuth != nullptr)
	{
		for (HTTPHeader header : *g_rghdGFLBansAuth)
		{
			Message("Header - %s: %s\n", header.GetName(), header.GetValue());
		}
	}
	Message((infPunishment->CreateInfractionJSON().dump(1) + "\n").c_str());
#endif
	g_HTTPManager.POST((g_strGFLBansApiUrl + "infractions/").c_str(),
					   infPunishment->CreateInfractionJSON().dump().c_str(),
					   [infPunishment, plyBadPerson, pAdmin](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
		Message(("Create Infraction Response:\n" + response.dump(1) + "\n").c_str());
#endif
		if (infPunishment->IsSession())
		// Session punishments don't care about response, since GFLBans instantly expires them
			return;

		if (!plyBadPerson || plyBadPerson->IsFakeClient() || !plyBadPerson->IsAuthenticated())
		{
			// This should only be hit if the player disconnected in the time between the query
			// being sent and GFLBans responding to the query
			ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "The player is not on the server...");
			return;
		}

		std::time_t iDuration = response.value("time_left", 0);
		if (iDuration == 0)
		{
			iDuration = response.value("expires", 0);
			if (iDuration != 0)
				iDuration -= response.value("created", 0);
		}
		iDuration = static_cast<int>(std::ceil(iDuration / 60.0)); //Convert from seconds to minutes

		CInfractionBase* infraction;
		std::string strPunishment = "";
		switch (infPunishment->GetInfractionType())
		{
			case GFLBans_InfractionBase::GFLInfractionType::Mute:
				strPunishment = "muted";
				infraction = new CMuteInfraction(iDuration, plyBadPerson->GetSteamId64());
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Gag:
				strPunishment = "gagged";
				infraction = new CGagInfraction(iDuration, plyBadPerson->GetSteamId64());
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Ban:
				strPunishment = "banned";
				infraction = new CBanInfraction(iDuration, plyBadPerson->GetSteamId64());
				break;
			default:
				// This should never be reached, since we it means we are trying to apply an unimplemented block type
				ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "Improper block type... Send to a dev with the command used.");
				return;
		}

		std::string strBadPlyName = CCSPlayerController::FromSlot(plyBadPerson->GetPlayerSlot())->GetPlayerName();

		if (iDuration == 0)
			strPunishment = "\2permanently\1 " + strPunishment + " " + strBadPlyName;
		else
			strPunishment.append(" " + strBadPlyName + " for \2" + FormatTime(iDuration, false) + "\1");

		if (infPunishment->GetReason() != "No reason provided")
			strPunishment.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");

		const char* pszAdminName = pAdmin ? pAdmin->GetPlayerName() : "Console";
		ClientPrintAll(HUD_PRINTTALK, CHAT_PREFIX ADMIN_PREFIX "%s%s%s.", pszAdminName, "", strPunishment.c_str(), "");

		// We're overwriting the infraction, so remove the previous one first
		g_pAdminSystem->FindAndRemoveInfraction(plyBadPerson, infraction->GetType(), false);
		g_pAdminSystem->AddInfraction(infraction);
		infraction->ApplyInfraction(plyBadPerson);
	}, g_rghdGFLBansAuth);
}

// Send a POST request to GFLBans telling it to remove all blocks of a given type for a player
// https://github.com/GFLClan/sm_gflbans/wiki/Implementer's-Guide-to-the-GFLBans-API#removing-infractions
void CAdminSystem::GFLBans_RemoveInfraction(std::shared_ptr<GFLBans_RemoveInfractionsOfPlayer> infPunishment,
											ZEPlayer* plyBadPerson, CCSPlayerController* pAdmin)
{
	if (std::time(nullptr) - m_wLastHeartbeat > 120)
	{
	//GFLBans has not responded for 2+ heartbeats. Remove punishment on server, but it will be
	// automatically reapplied when GFLBans comes back up if not removed on the web
		bool bIsPunished = false;
		switch (infPunishment->GetInfractionType())
		{
			case GFLBans_InfractionBase::GFLInfractionType::Mute:
				bIsPunished = plyBadPerson->IsMuted();
				RemoveInfractionType(plyBadPerson, CInfractionBase::EInfractionType::Mute, false);
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Gag:
				bIsPunished = plyBadPerson->IsGagged();
				RemoveInfractionType(plyBadPerson, CInfractionBase::EInfractionType::Gag, false);
				break;
		}
		if (bIsPunished)
			ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "Local block removed, but \2GFLBans is currently down\1. Any web blocks will be reapplied when GFLBans comes back online.");
	}

#ifdef _DEBUG
	Message(("Remove Infraction Query:\n" + g_strGFLBansApiUrl + "infractions/remove\n").c_str());
	if (g_rghdGFLBansAuth != nullptr)
	{
		for (HTTPHeader header : *g_rghdGFLBansAuth)
		{
			Message("Header - %s: %s\n", header.GetName(), header.GetValue());
		}
	}
	Message((infPunishment->CreateInfractionJSON().dump(1) + "\n").c_str());
#endif
	g_HTTPManager.POST((g_strGFLBansApiUrl + "infractions/remove").c_str(),
					   infPunishment->CreateInfractionJSON().dump().c_str(),
					   [infPunishment, plyBadPerson, pAdmin](HTTPRequestHandle request, json response) {

#ifdef _DEBUG
		Message(("Remove Infraction Response:\n" + response.dump(1) + "\n").c_str());
#endif
		int iRemovedBlocks = response.value("num_removed", 0);

		if (!plyBadPerson)
		{
		// This should only be hit if the player disconnected in the time between the query being sent
		// and GFLBans responding to the query
			ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "The player is not on the server...");
			return;
		}
		// Invalidate local punishments of infraction's type
		CInfractionBase::EInfractionType itypeToRemove;
		bool bRemoveGagAndMute = false;
		std::string strPunishment = "";
		bool bIsPunished = false;
		switch (infPunishment->GetInfractionType())
		{
			case GFLBans_InfractionBase::GFLInfractionType::Mute:
				bIsPunished = plyBadPerson->IsMuted();
				strPunishment = "muted";
				itypeToRemove = CInfractionBase::EInfractionType::Mute;
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Gag:
				bIsPunished = plyBadPerson->IsGagged();
				strPunishment = "gagged";
				itypeToRemove = CInfractionBase::EInfractionType::Gag;
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Ban:
				// This should never be hit, since plyBadPerson wouldn't be valid (connected) if they were banned
				strPunishment = "banned";
				itypeToRemove = CInfractionBase::EInfractionType::Ban;
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Silence:
				strPunishment = "silenced";
				bIsPunished = plyBadPerson->IsGagged() || plyBadPerson->IsMuted();
				bRemoveGagAndMute = true;
				break;
			default:
				return;
		}

		if (!bIsPunished)
		{
			strPunishment = " is not " + strPunishment + ".";
			strPunishment = CCSPlayerController::FromSlot(plyBadPerson->GetPlayerSlot())->GetPlayerName() + strPunishment;
			if (pAdmin)
				ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "%s", strPunishment.c_str());
			else
				Message(strPunishment.c_str());

			return;
		}

		if (infPunishment->GetReason() != "No reason provided")
			strPunishment.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");
		
		strPunishment = "un" + strPunishment;
		const char* pszCommandPlayerName = pAdmin ? pAdmin->GetPlayerName() : "Console";
		PrintSingleAdminAction(pszCommandPlayerName, CCSPlayerController::FromSlot(plyBadPerson->GetPlayerSlot())->GetPlayerName(),
							   strPunishment.c_str());

		g_pAdminSystem->RemoveInfractionType(plyBadPerson, itypeToRemove, bRemoveGagAndMute);
	}, g_rghdGFLBansAuth);
}

void CAdminSystem::RemoveInfractionType(ZEPlayer* player, CInfractionBase::EInfractionType itypeToRemove, bool bRemoveGagAndMute)
{
	if (!player || player->IsFakeClient())
		return;

	if (bRemoveGagAndMute)
	{
		player->SetMuted(false);
		player->SetGagged(false);
	}
	else
	{
		switch (itypeToRemove)
		{
			case CInfractionBase::EInfractionType::Mute:
				player->SetMuted(false);
				break;
			case CInfractionBase::EInfractionType::Gag:
				player->SetGagged(false);
				break;
		}
	}

	FOR_EACH_VEC(m_vecInfractions, i)
	{
		uint64 iSteamID = player->IsAuthenticated() ? player->GetSteamId64() : player->GetUnauthenticatedSteamId64();

		// We're only interested in infractions concerning this player
		if (m_vecInfractions[i]->GetSteamId64() != iSteamID)
			continue;

		// We only care about removing infractions of the given type(s)
		if ((bRemoveGagAndMute && (m_vecInfractions[i]->GetType() == CInfractionBase::EInfractionType::Mute ||
								   m_vecInfractions[i]->GetType() == CInfractionBase::EInfractionType::Gag)) ||
			m_vecInfractions[i]->GetType() == itypeToRemove)
		{
			m_vecInfractions.Remove(i);
		}
	}

	// Check GFLBans for any other infractions on player and apply them if they exist
	GFLBans_CheckPlayerInfractions(player);
}

bool CAdminSystem::CheckJSONForBlock(ZEPlayer* player, json jAllBlockInfo,
									 GFLBans_InfractionBase::GFLInfractionType blockType,
									 bool bApplyBlock, bool bRemoveSession)
{
	if (!player || player->IsFakeClient())
		return false;
	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(player->GetPlayerSlot());

	if (!pTarget)
		return false;

	std::string strBlockType;

	switch (blockType)
	{
		case GFLBans_InfractionBase::GFLInfractionType::Mute:
			strBlockType = "voice_block";
			break;
		case GFLBans_InfractionBase::GFLInfractionType::Gag:
			strBlockType = "chat_block";
			break;
		case GFLBans_InfractionBase::GFLInfractionType::Ban:
			strBlockType = "ban";
			break;
		//case GFLBans_InfractionBase::GFLInfractionType::AdminChatGag:
		//	strBlockType = "admin_chat_block";
		//	break;
		default:
		// We dont need to check to apply a Call Admin Block server side, since that is all handled by GFLBans itself
			return false;
	}

	json jBlockInfo = jAllBlockInfo.value(strBlockType, json());
	if (jBlockInfo.empty())
	{
		if (player->IsAuthenticated())
		{
			switch (blockType)
			{
				case GFLBans_InfractionBase::GFLInfractionType::Mute:
					g_pAdminSystem->FindAndRemoveInfraction(player, CInfractionBase::EInfractionType::Mute, bRemoveSession);
					break;
				case GFLBans_InfractionBase::GFLInfractionType::Gag:
					g_pAdminSystem->FindAndRemoveInfraction(player, CInfractionBase::EInfractionType::Gag, bRemoveSession);
					break;
				case GFLBans_InfractionBase::GFLInfractionType::Ban:
					g_pAdminSystem->FindAndRemoveInfraction(player, CInfractionBase::EInfractionType::Ban, bRemoveSession);
					break;
			}
		}

		return false;
	}


	if (bApplyBlock)
	{
		time_t iDuration = jBlockInfo.value("expiration", std::time(nullptr)) - std::time(nullptr);
		iDuration = static_cast<time_t>(std::ceil(iDuration / 60.0));

		uint64 iSteamID = player->IsAuthenticated() ? player->GetSteamId64() : player->GetUnauthenticatedSteamId64();

		CInfractionBase* infraction;
		switch (blockType)
		{
			case GFLBans_InfractionBase::GFLInfractionType::Mute:
				infraction = new CMuteInfraction(iDuration, iSteamID);
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Gag:
				infraction = new CGagInfraction(iDuration, iSteamID);
				break;
			case GFLBans_InfractionBase::GFLInfractionType::Ban:
				infraction = new CBanInfraction(iDuration, iSteamID);
				break;
			default:
				return true;
		}

		// Overwrite any existing infractions of the same type and update from web
		// This is in case a current infraction was edited on the web
		g_pAdminSystem->FindAndRemoveInfraction(player, infraction->GetType(), bRemoveSession);
		g_pAdminSystem->AddInfraction(infraction);
		infraction->ApplyInfraction(player);
	}

	return true;
}

void CAdminSystem::AddDisconnectedPlayer(const char* pszName, uint64 xuid, const char* pszIP)
{
	// Remove all non-session infractions for a player when they disconnect, since these should be
	// queried for again when the player rejoins
	FOR_EACH_VEC_BACK(m_vecInfractions, i)
	{
		if (m_vecInfractions[i]->GetSteamId64() == xuid && !m_vecInfractions[i]->IsSession())
			m_vecInfractions.Remove(i);
	}

	auto plyInfo = std::make_tuple(pszName, xuid, pszIP);
	for (auto& dcPlyInfo : m_rgDCPly)
	{
		if (std::get<1>(dcPlyInfo) == std::get<1>(plyInfo))
			return;
	}
	m_rgDCPly[m_iDCPlyIndex] = plyInfo;
	m_iDCPlyIndex = (m_iDCPlyIndex + 1) % 20;
}

void CAdminSystem::ShowDisconnectedPlayers(CCSPlayerController* const pAdmin)
{
	bool bAnyDCedPlayers = false;
	for (int i = 1; i <= 20; i++)
	{
		int index = (m_iDCPlyIndex - i) % 20;
		if (index < 0)
			index += 20;
		std::tuple<std::string, uint64, std::string> ply = m_rgDCPly[index];
		if (std::get<1>(ply) != 0)
		{
			if (!bAnyDCedPlayers)
			{
				if (pAdmin)
					ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "Disconnected player(s) displayed in console.");
				ClientPrint(pAdmin, HUD_PRINTCONSOLE, "Disconnected Player(s):");
				bAnyDCedPlayers = true;
			}

			std::string strTemp = std::get<0>(ply) + "\n\tSteam64 ID - " + std::to_string(std::get<1>(ply)) + "\n\tIP Address - " + std::get<2>(ply);
			ClientPrint(pAdmin, HUD_PRINTCONSOLE, "%i. %s", i, strTemp.c_str());
		}
	}
	if (!bAnyDCedPlayers)
		ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "No players have disconnected yet.");
}

inline bool CAdminSystem::CanPunishmentBeOffline(int iDuration) const noexcept
{
	return iDuration >= g_iMinOfflineDurations || g_iMinOfflineDurations <= 0;
}


void ConsoleListPunishments(CCSPlayerController* const player, json punishments)
{
	if (!player)
		return;

	for (const auto& punishment : punishments.items())
	{
		time_t wExpiration = punishment.value().value("expiration", -1);
		std::string strPunishmentType;

		if (punishment.key() == "voice_block")
			strPunishmentType = "Muted";
		else if (punishment.key() == "chat_block")
			strPunishmentType = "Gagged";
		else if (punishment.key() == "ban")
			strPunishmentType = "Banned";
		else if (punishment.key() == "admin_chat_block")
			strPunishmentType = "Admin Chat Blocked";
		else if (punishment.key() == "call_admin_block")
			strPunishmentType = "Call Admin Blocked";
		else
			strPunishmentType = punishment.key();

		if (wExpiration <= 0)
			strPunishmentType = "Permanently " + strPunishmentType + ":";
		else
			strPunishmentType = strPunishmentType + " for " + FormatTime(wExpiration - std::time(nullptr)) + ":";

		ClientPrint(player, HUD_PRINTCONSOLE, strPunishmentType.c_str());

		for (const auto& val : punishment.value().items())
		{
			std::string desc;
			if (val.key() == "expiration")
				continue;
			else if (val.key() == "admin_name")
				desc = "\tAdmin: ";
			else if (val.key() == "reason")
				desc = "\tReason: ";
			else
				desc = "\t" + val.key() + ": ";
			std::string temp = val.value().dump();
			desc.append(temp.substr(1, temp.length() - 2));

			ClientPrint(player, HUD_PRINTCONSOLE, desc.c_str());
		}
	}
}

std::string FormatTime(std::time_t wTime, bool bInSeconds)
{
	if (bInSeconds)
	{
		if (wTime < 60)
			return std::to_string(static_cast<int>(std::floor(wTime))) + " second" + (wTime >= 2 ? "s" : "");
		wTime = wTime / 60;
	}

	if (wTime < 60)
		return std::to_string(static_cast<int>(std::floor(wTime))) + " minute" + (wTime >= 2 ? "s" : "");
	wTime = wTime / 60;

	if (wTime < 24)
		return std::to_string(static_cast<int>(std::floor(wTime))) + " hour" + (wTime >= 2 ? "s" : "");
	wTime = wTime / 24;

	if (wTime < 7)
		return std::to_string(static_cast<int>(std::floor(wTime))) + " day" + (wTime >= 2 ? "s" : "");
	wTime = wTime / 7;

	if (wTime < 4)
		return std::to_string(static_cast<int>(std::floor(wTime))) + " week" + (wTime >= 2 ? "s" : "");
	wTime = wTime / 4;

	return std::to_string(static_cast<int>(std::floor(wTime))) + " month" + (wTime >= 2 ? "s" : "");
}

int ParseTimeInput(std::string strTime)
{
	if (strTime.length() == 0 || std::find_if(strTime.begin(), strTime.end(), [](char c) { return c == '-'; }) != strTime.end())
		return -1;

	std::string strNumbers = "";
	std::copy_if(strTime.begin(), strTime.end(), std::back_inserter(strNumbers), [](char c) { return std::isdigit(c); });

	if (strNumbers.length() == 0)
		return -1;
	else if (strNumbers.length() > 9)
	// Really high number, just return perma
		return 0;

	// stoi should be exception safe here due to above checks
	int iDuration = std::stoi(strNumbers.c_str());

	if (iDuration == 0)
		return 0;
	else if (iDuration < 0)
		return -1;

	switch (strTime[strTime.length() - 1])
	{
		case 'h':
		case 'H':
			return iDuration * 60.0 > INT_MAX ? 0 : iDuration * 60;
			break;
		case 'd':
		case 'D':
			return iDuration * 60.0 * 24.0 > INT_MAX ? 0 : iDuration * 60 * 24;
			break;
		case 'w':
		case 'W':
			return iDuration * 60.0 * 24.0 * 7.0 > INT_MAX ? 0 : iDuration * 60 * 24 * 7;
			break;
		case 'm':
		case 'M':
			return iDuration * 60.0 * 24.0 * 7.0 * 4.0 > INT_MAX ? 0 : iDuration * 60 * 24 * 7 * 4;
			break;
		default:
			return iDuration;
	}
}