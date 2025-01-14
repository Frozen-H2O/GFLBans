/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023-2024 Source2ZE
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

#include "gflbans.h"

#include "adminsystem.h"
#include "commands.h"
#include "ctimer.h"
#include "entity/ccsplayercontroller.h"
#include "entity/cgamerules.h"
#include "httpmanager.h"
#include "vendor/nlohmann/json.hpp"
#include <regex>

using json = nlohmann::json;

extern IVEngineServer2* g_pEngineServer2;
extern CGameEntitySystem* g_pEntitySystem;
extern CGlobalVars* gpGlobals;
extern CCSGameRules* g_pGameRules;
extern CAdminSystem* g_pAdminSystem;

GFLBansSystem* g_pGFLBansSystem = nullptr;

void ParseInfraction(const CCommand& args, CCSPlayerController* pAdmin, bool bAdding, InfType infType);
void ConsoleListPunishments(CCSPlayerController* const player, json punishments);
const char* GetActionPhrase(InfType typeInfraction, GrammarTense iTense, bool bAdding = true);
const char* LocalToWebInfraction(InfType typeInfraction);
json PlayerObj(CHandle<CCSPlayerController> hPlayer, bool bUseIP);
json PlayerObj(CCSPlayerController* pPlayer, bool bUseIP);
json PlayerObj(ZEPlayer* zpPlayer, bool bUseIP);
std::string PlayerQuery(ZEPlayer* zpPlayer, bool bUseIP);
bool IsValidIP(std::string strIP);

// --- Convars ---
static std::string g_strGFLBansApiUrl = "https://bans.gflclan.com/api/";
static bool g_bGFLBansIssueGlobal = true;
static bool g_bGFLBansAcceptGlobal = true;
static int g_iMinRealWorldDuration = 61;
static int g_bFilterGagDuration = 60;
static std::string g_strGFLBansHostname = "CS2 ZE Test";
static std::string g_strGFLBansServerID = "999";
static std::string g_strGFLBansServerKey = "1337";
static std::vector<HTTPHeader>* g_rghdGFLBansAuth = new std::vector<HTTPHeader>{HTTPHeader("Authorization", "SERVER " + g_strGFLBansServerID + " " + g_strGFLBansServerKey)};
static std::string g_strChatFilter = "n+i+g+e+r+";
static std::regex g_regChatFilter(g_strChatFilter, std::regex_constants::ECMAScript | std::regex_constants::icase);
static std::string g_strChatFilterNoPunish = "nigga";
static std::regex g_regChatFilterNoPunish(g_strChatFilterNoPunish, std::regex_constants::ECMAScript | std::regex_constants::icase);

// This only affects the CURRENT report being sent and is not logged in GFLBans. This means that
// if a report was sent 1 minute ago with a cooldown of 600 seconds, but a new report is sent with a
// 20 second cooldown, the new report will be successfull. So for an emergency report that you need
// to force through, set this to 1, send the report, then change it back to the original value
static int g_iGFLBansReportCooldown = 600;

FAKE_STRING_CVAR(gflbans_api_url, "URL to interact with GFLBans API. Should end in \"api/\"", g_strGFLBansApiUrl, false)
FAKE_STRING_CVAR(gflbans_hostname, "Name of the server", g_strGFLBansHostname, false) // remove once we can read hostname convar
FAKE_BOOL_CVAR(gflbans_issue_global, "Infractions on the server will be made global if the server admin has permission to set global punishments.", g_bGFLBansIssueGlobal, true, false)
FAKE_BOOL_CVAR(gflbans_accept_global, "Whether globally issued punishments apply to this server or not.", g_bGFLBansAcceptGlobal, true, false)
FAKE_INT_CVAR(gflbans_filtered_gag_duration, "Minutes to gag a player if they type a filtered message. Gags will only be issued with non-negative values", g_bFilterGagDuration, 60, false)
FAKE_INT_CVAR(gflbans_min_real_world_timed, "Minimum amount of minutes for a mute/gag duration to be real world timed. 0 or negative values force all punishments to be game timed", g_iMinRealWorldDuration, 61, false)
FAKE_INT_CVAR(gflbans_report_cooldown, "Minimum amount of seconds between c_report/c_calladmin usages. Minimum of 1 second.", g_iGFLBansReportCooldown, 600, false)

// These need to update g_rghdGFLBansAuth when changed, but otherwise work just like a fake cvar as if they weren't commented out:
//  FAKE_STRING_CVAR(gflbans_server_id, "GFLBans ID for the server.", g_strGFLBansServerID, true)
CON_COMMAND_F(gflbans_server_id, "GFLBans ID for the server.", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY | FCVAR_PROTECTED)
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

CON_COMMAND_F(gflbans_filter_regex, "<regex> - sets the basic_regex (case insensitive) to delete any chat messages containing a match and punish the player that typed them", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY | FCVAR_PROTECTED)
{
	if (args.ArgC() < 2)
	{
		Msg("%s %s\n", args[0], g_strChatFilter.c_str());
		return;
	}
	g_strChatFilter = args[1];
	g_regChatFilter = std::regex(g_strChatFilter, std::regex_constants::ECMAScript | std::regex_constants::icase);
}

CON_COMMAND_F(gflbans_filter_no_punish_regex, "<regex> - sets the basic_regex (case insensitive) to delete any chat messages containing a match", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY | FCVAR_PROTECTED)
{
	if (args.ArgC() < 2)
	{
		Msg("%s %s\n", args[0], g_strChatFilterNoPunish.c_str());
		return;
	}
	g_strChatFilterNoPunish = args[1];
	g_regChatFilterNoPunish = std::regex(g_strChatFilterNoPunish, std::regex_constants::ECMAScript | std::regex_constants::icase);
}

// --- Commands ---
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

CON_COMMAND_CHAT_FLAGS(ban, "<name> <duration> [reason] - Ban a player", ADMFLAG_BAN)
{
	ParseInfraction(args, player, true, Ban);
}

CON_COMMAND_CHAT_FLAGS(mute, "<name> [(+)duration] [reason] - Mute a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, Mute);
}

CON_COMMAND_CHAT_FLAGS(unmute, "<name> [reason] - Unmute a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, Mute);
}

CON_COMMAND_CHAT_FLAGS(gag, "<name> [(+)duration] [reason] - Gag a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, Gag);
}

CON_COMMAND_CHAT_FLAGS(ungag, "<name> [reason] - Ungag a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, Gag);
}

CON_COMMAND_CHAT_FLAGS(silence, "<name> [(+)duration] [reason] - Mute and gag a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, Silence);
}

CON_COMMAND_CHAT_FLAGS(unsilence, "<name> [reason] - Unmute and ungag a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, Silence);
}

CON_COMMAND_CHAT_FLAGS(admingag, "<name> [(+)duration] [reason] - Gag a player from using adminchat", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, AdminChatGag);
}

CON_COMMAND_CHAT_FLAGS(unadmingag, "<name> [reason] - Ungag a player from using adminchat", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, AdminChatGag);
}

CON_COMMAND_CHAT_FLAGS(adminungag, "<name> [reason] - Ungag a player from using adminchat", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, AdminChatGag);
}

CON_COMMAND_CHAT_FLAGS(callban, "<name> <(+)duration> [reason] - Ban a player from using report and calladmin", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, CallAdminBlock);
}

CON_COMMAND_CHAT_FLAGS(uncallban, "<name> [reason] - Unban a player from using report and calladmin", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, CallAdminBlock);
}

CON_COMMAND_CHAT_FLAGS(callunban, "<name> [reason] - Unban a player from using report and calladmin", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, CallAdminBlock);
}

CON_COMMAND_CHAT_FLAGS(history, "<name> - Checks a player's infraction history", ADMFLAG_GENERIC)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Usage: !history <name>");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_RANDOM | NO_MULTIPLE | NO_BOT | NO_UNAUTHENTICATED))
		return;

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[0]);
	g_pGFLBansSystem->CheckPunishmentHistory(player, pTarget, GetReason(args, 1, true));
}

CON_COMMAND_CHAT(report, "<name> <reason> - report a player")
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Usage: /report <name> <reason>");
		return;
	}

	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "A calling player is required by GFLBans, so you may not report through console. Use \"c_info <name>\" to find their information instead.");
		return;
	}

	ZEPlayer* zpPlayer = player->GetZEPlayer();
	if (!zpPlayer->IsAuthenticated())
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You are not authenticated yet. Please wait a bit and try again.");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_RANDOM | NO_MULTIPLE | NO_SELF | NO_BOT | NO_UNAUTHENTICATED | NO_IMMUNITY))
		return;

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[0]);
	ZEPlayer* zpTarget = pTarget->GetZEPlayer();

	std::string strMessage = GetReason(args, 1, true);

	if (strMessage.length() <= 0)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You must provide a reason for reporting.");
		return;
	}

	ClientPrint(player, HUD_PRINTTALK, " \7[GFLBans]\x0B Attempting to report \2%s \x0B(reason: \x09%s\x0B)...", pTarget->GetPlayerName(), strMessage.c_str());
	ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Type \x0E/confirm\1 within 30 seconds to send your pending report. Issuing false reports will result in a\x02 ban\1.");
	uint64 reportIndex = zpPlayer->GetSteamId64();
	if (g_pGFLBansSystem->mapPendingReports.find(reportIndex) != g_pGFLBansSystem->mapPendingReports.end())
	{
		for (int i = g_timers.Tail(); i != g_timers.InvalidIndex();)
		{
			auto timer = g_timers[i];

			int prevIndex = i;
			i = g_timers.Previous(i);

			// Delete existing timer
			if (timer == g_pGFLBansSystem->mapPendingReports[reportIndex].second)
			{
				delete timer;
				g_timers.Remove(prevIndex);
				break;
			}
		}
	}

	int iCommandPlayerSlot = player->GetPlayerSlot();
	CTimerBase* timer = new CTimer(30.0f, true, true, [reportIndex, iCommandPlayerSlot]() {
		auto player = CCSPlayerController::FromSlot(iCommandPlayerSlot);
		if (g_pGFLBansSystem->mapPendingReports.find(reportIndex) != g_pGFLBansSystem->mapPendingReports.end())
		{
			g_pGFLBansSystem->mapPendingReports.erase(reportIndex);
#if _DEBUG
			Message("Deleted a report/admin call");
#endif
			if (!player)
				return -1.0f;

			ZEPlayer* zpPlayer = player->GetZEPlayer();

			if (!zpPlayer || zpPlayer->IsFakeClient())
				return -1.0f;

			if (zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64() == reportIndex)
				ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Your admin call has been cancelled due to not using \x0E/confirm\1 within 30 seconds.");
		}
		return -1.0f;
	});

	g_pGFLBansSystem->mapPendingReports[reportIndex] =
		std::make_pair(std::make_shared<GFLBans_Report>(player, strMessage, pTarget), timer);
}

CON_COMMAND_CHAT(calladmin, "<reason> - request for an admin to join the server")
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Usage: /calladmin <reason>");
		return;
	}

	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "A calling player is required by GFLBans, so you may not call admins through console.");
		return;
	}

	ZEPlayer* zpPlayer = player->GetZEPlayer();
	if (!zpPlayer->IsAuthenticated())
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You are not authenticated yet. Please wait a bit and try again.");
		return;
	}

	std::string strMessage = GetReason(args, 0, true);
	if (strMessage.length() <= 0)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You must provide a reason for calling an admin.");
		return;
	}

	ClientPrint(player, HUD_PRINTTALK, " \7[GFLBans]\x0B Attempting to call an admin (reason: \x09%s\x0B)...", strMessage.c_str());
	ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Type \x0E/confirm\1 within 30 seconds to send your pending admin call. Abusing this feature will result in a\x02 ban\1.");
	uint64 reportIndex = zpPlayer->GetSteamId64();
	if (g_pGFLBansSystem->mapPendingReports.find(reportIndex) != g_pGFLBansSystem->mapPendingReports.end())
	{
		for (int i = g_timers.Tail(); i != g_timers.InvalidIndex();)
		{
			auto timer = g_timers[i];

			int prevIndex = i;
			i = g_timers.Previous(i);

			// Delete existing timer
			if (timer == g_pGFLBansSystem->mapPendingReports[reportIndex].second)
			{
				delete timer;
				g_timers.Remove(prevIndex);
				break;
			}
		}
	}

	int iCommandPlayerSlot = player->GetPlayerSlot();
	CTimerBase* timer = new CTimer(30.0f, true, true, [reportIndex, iCommandPlayerSlot]() {
		auto player = CCSPlayerController::FromSlot(iCommandPlayerSlot);
		if (g_pGFLBansSystem->mapPendingReports.find(reportIndex) != g_pGFLBansSystem->mapPendingReports.end())
		{
			g_pGFLBansSystem->mapPendingReports.erase(reportIndex);
#if _DEBUG
			Message("Deleted a report/admin call");
#endif
			if (!player)
				return -1.0f;

			ZEPlayer* zpPlayer = player->GetZEPlayer();

			if (!zpPlayer || zpPlayer->IsFakeClient())
				return -1.0f;

			if (zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64() == reportIndex)
				ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Your admin call has been cancelled due to not using \x0E/confirm\1 within 30 seconds.");
		}
		return -1.0f;
	});

	g_pGFLBansSystem->mapPendingReports[reportIndex] =
		std::make_pair(std::make_shared<GFLBans_Report>(player, strMessage), timer);
}

CON_COMMAND_CHAT(confirm, "- send a report or admin call that you attempted to send within the last 30 seconds")
{
	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "A calling player is required by GFLBans, so reports can not be sent through console.");
		return;
	}

	ZEPlayer* zpPlayer = player->GetZEPlayer();

	if (!zpPlayer->IsAuthenticated())
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You are not authenticated yet. Please wait a bit and try again.");
		return;
	}

	if (g_pGFLBansSystem->mapPendingReports.find(zpPlayer->GetSteamId64()) == g_pGFLBansSystem->mapPendingReports.end())
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You do not have any pending reports or admin calls. Please create one with \x02/report\1 or \x02/calladmin\1 before using \x0E/confirm\1");
		return;
	}

	for (int i = g_timers.Tail(); i != g_timers.InvalidIndex();)
	{
		auto timer = g_timers[i];

		int prevIndex = i;
		i = g_timers.Previous(i);

		// Delete existing timer
		if (timer == g_pGFLBansSystem->mapPendingReports[zpPlayer->GetSteamId64()].second)
		{
			delete timer;
			g_timers.Remove(prevIndex);
			break;
		}
	}

	std::shared_ptr<GFLBans_Report> report = g_pGFLBansSystem->mapPendingReports[zpPlayer->GetSteamId64()].first;
	g_pGFLBansSystem->mapPendingReports.erase(zpPlayer->GetSteamId64());
	report->GFLBans_CallAdmin(player);
}

CON_COMMAND_CHAT_FLAGS(claim, "- claims the most recent GFLBans report/calladmin query", ADMFLAG_KICK)
{
	json jClaim;
	jClaim["admin_name"] = player ? player->GetPlayerName() : "SYSTEM";

	if (std::time(nullptr) - g_pGFLBansSystem->m_wLastHeartbeat > 120)
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "GFLBans is currently not responding. Your claim may fail.");

	CHandle<CCSPlayerController> hPlayer = player->GetHandle();

#ifdef _DEBUG
	Message(("Claim Query:\n" + g_strGFLBansApiUrl + "gs/calladmin/claim\n").c_str());
	if (g_rghdGFLBansAuth != nullptr)
		for (HTTPHeader header : *(g_rghdGFLBansAuth))
			Message("Header - %s: %s\n", header.GetName(), header.GetValue());
	Message((jClaim.dump(1) + "\n").c_str());
#endif

	g_HTTPManager.POST(
		(g_strGFLBansApiUrl + "gs/calladmin/claim").c_str(),
		jClaim.dump().c_str(),
		[hPlayer](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
			Message(("Claim Response:\n" + response.dump(1) + "\n").c_str());
#endif

			CCSPlayerController* player = hPlayer.Get();

			if (!player)
				return;

			if (!response.value("success", false))
			{
				if (response.value("msg", "").length() > 0)
					ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "%s", response.value("msg", "").c_str());
				else
					ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Claim request failed. Are you sure there is an open admin call?");
			}
			else
			{
				if (response.value("msg", "").length() > 0)
					ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "%s", response.value("msg", "").c_str());
				else
					ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Successfully claimed an admin call.");
			}
		},
		g_rghdGFLBansAuth);
}

CON_COMMAND_CHAT(status, "<name> - List a player's active punishments. Non-admins may only check their own punishments")
{
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];
	ETargetType nType;
	ZEPlayer* pTargetPlayer = nullptr;
	bool bIsAdmin = !player || player->GetZEPlayer()->IsAdminFlagSet(ADMFLAG_GENERIC);
	std::string strTarget = (!bIsAdmin || args.ArgC() < 2) ? "@me" : args[1];

	if (!g_playerManager->CanTargetPlayers(player, strTarget.c_str(), iNumClients, pSlots, NO_UNAUTHENTICATED | NO_MULTIPLE | NO_BOT, nType))
		return;

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[0]);
	bool bSelfTarget = pTarget == player;

	// Send the requests
	std::string strURL = PlayerQuery(pTarget->GetZEPlayer(), false);

	if (strURL.length() == 0)
		return;

	CHandle<CCSPlayerController> hPlayer = player ? player->GetHandle() : nullptr;
	CHandle<CCSPlayerController> hTarget = pTarget->GetHandle();

	g_HTTPManager.GET(
		strURL.c_str(),
		[hPlayer, hTarget, strTarget, bSelfTarget](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
			Message("status response: %s\n", response.dump().c_str());
#endif
			CCSPlayerController* pPlayer = hPlayer ? hPlayer.Get() : nullptr;
			CCSPlayerController* pTarget = hTarget.Get();
			if (!pTarget)
			{
				ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "Target not found.");
				return;
			}

			ZEPlayer* zpTarget = pTarget->GetZEPlayer();
			if (!zpTarget)
			{
				ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "Target not found.");
				return;
			}

			std::vector<std::string> rgstrPunishments;
			if (zpTarget->IsGagged())
				rgstrPunishments.push_back(GetActionPhrase(Gag, GrammarTense::Past, true));
			if (zpTarget->IsMuted())
				rgstrPunishments.push_back(GetActionPhrase(Mute, GrammarTense::Past, true));
			if (zpTarget->IsAdminChatGagged())
				rgstrPunishments.push_back(GetActionPhrase(AdminChatGag, GrammarTense::Past, true));
			if (response.contains("call_admin_block"))
				rgstrPunishments.push_back(GetActionPhrase(CallAdminBlock, GrammarTense::Past, true));

			std::string strPunishment = "";
			if (rgstrPunishments.size() == 1)
				strPunishment = "\2" + rgstrPunishments[0] + "\1";
			else if (rgstrPunishments.size() == 2)
				strPunishment = "\2" + rgstrPunishments[0] + "\1 and\2 " + rgstrPunishments[1] + "\1";
			else if (rgstrPunishments.size() > 2)
			{
				for (int i = 0; i < rgstrPunishments.size() - 1; i++)
					strPunishment.append("\2" + rgstrPunishments[i] + "\1, ");
				strPunishment.append("and\2 " + rgstrPunishments[rgstrPunishments.size() - 1] + "\1");
			}

			if (response.dump().length() < 5)
			{
				if (strPunishment.length() > 0)
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "%s %s.",
								bSelfTarget ? "You are" : (strTarget + " is").c_str(),
								strPunishment.c_str());
				else
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "%s no active punishments.",
								bSelfTarget ? "You have" : (strTarget + " has").c_str());
				return;
			}

			ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "%s currently %s. Check console for more information.",
						bSelfTarget ? "You are" : (strTarget + " is").c_str(), strPunishment.c_str());

			if (bSelfTarget)
				ClientPrint(pPlayer, HUD_PRINTCONSOLE, "[GFLBans] Your active punishments:");
			else
				ClientPrint(pPlayer, HUD_PRINTCONSOLE, "[GFLBans] Active punishments for %s:", (strTarget).c_str());

			ConsoleListPunishments(pPlayer, response);
		},
		g_rghdGFLBansAuth);
}

// --- GFLBans Spaghetti ---
GFLBans_InfractionBase::GFLBans_InfractionBase(InfType infType, CHandle<CCSPlayerController> hTarget,
											   std::string strReason, CHandle<CCSPlayerController> hAdmin) :
	m_infType(infType), m_hTarget(hTarget), m_hAdmin(hAdmin)
{
	m_strReason = strReason.length() == 0	? "No reason provided" :
				  strReason.length() <= 280 ? strReason :
											  strReason.substr(0, 280);
}

GFLBans_Infraction::GFLBans_Infraction(InfType infType, CHandle<CCSPlayerController> hTarget, std::string strReason,
									   CHandle<CCSPlayerController> hAdmin, int iDuration, bool bOnlineOnly) :
	GFLBans_InfractionBase(infType, hTarget, strReason, hAdmin), m_bOnlineOnly(bOnlineOnly)
{
	m_wCreated = std::time(nullptr);
	m_wExpires = m_wCreated + (iDuration * 60);
	m_gisScope = g_bGFLBansIssueGlobal ? Global : Server;
}

inline bool GFLBans_Infraction::IsSession() const noexcept
{
	return m_wExpires < m_wCreated && m_infType != Ban;
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

	jRequestBody["player"] = PlayerObj(m_hTarget, true);

	// Omit admin for block through CONSOLE
	if (m_hAdmin != nullptr)
	{
		json jAdmin;
		jAdmin["gs_admin"] = PlayerObj(m_hAdmin, false);
		jRequestBody["admin"] = jAdmin;
	}

	jRequestBody["reason"] = m_strReason;

	json jPunishments = json::array();
	if (m_infType == Silence)
	{
		jPunishments[0] = "voice_block";
		jPunishments[1] = "chat_block";
	}
	else if (m_infType != Warn)
		jPunishments[0] = LocalToWebInfraction(m_infType);
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

	if (m_bOnlineOnly && m_infType != Ban && iDuration > 0)
		jRequestBody["dec_online_only"] = true;

	return jRequestBody;
}

json GFLBans_InfractionRemoval::CreateInfractionJSON() const
{
	json jRequestBody;

	jRequestBody["player"] = PlayerObj(m_hTarget, true);
	jRequestBody["remove_reason"] = m_strReason;

	// Omit admin for unblock through CONSOLE
	if (m_hAdmin != nullptr)
	{
		json jAdmin;
		jAdmin["gs_admin"] = PlayerObj(m_hAdmin, false);
		jRequestBody["admin"] = jAdmin;
	}

	// Omit for default true
	if (!g_bGFLBansAcceptGlobal)
		jRequestBody["include_other_servers"] = g_bGFLBansAcceptGlobal;

	json jPunishments = json::array();
	if (m_infType == Silence)
	{
		jPunishments[0] = "voice_block";
		jPunishments[1] = "chat_block";
	}
	else
		jPunishments[0] = LocalToWebInfraction(m_infType);
	jRequestBody["restrict_types"] = jPunishments;

	return jRequestBody;
}

GFLBans_Report::GFLBans_Report(CCSPlayerController* pCaller, std::string strMessage,
							   CCSPlayerController* pBadPerson)
{
	m_jCaller = PlayerObj(pCaller, false);
	m_strCallerName = pCaller ? pCaller->GetPlayerName() : "SYSTEM";
	m_strMessage = strMessage.length() == 0	  ? "No reason provided" :
				   strMessage.length() <= 120 ? strMessage :
												strMessage.substr(0, 120);
	m_jBadPerson = pBadPerson ? PlayerObj(pBadPerson, false) : json();
	m_strBadPersonName = pBadPerson ? pBadPerson->GetPlayerName() : "";
}

json GFLBans_Report::CreateReportJSON() const
{
	json jRequestBody;
	jRequestBody["caller"] = m_jCaller;
	jRequestBody["caller_name"] = m_strCallerName;

	// Omit for false
	if (g_bGFLBansAcceptGlobal)
		jRequestBody["include_other_servers"] = true;

	jRequestBody["message"] = m_strMessage;

	// Omit for 10 minutes
	if (g_iGFLBansReportCooldown != 600 && g_iGFLBansReportCooldown >= 0)
		jRequestBody["cooldown"] = g_iGFLBansReportCooldown;

	// Omit for generic call admin (no target of report)
	if (IsReport())
	{
		jRequestBody["report_target"] = m_jBadPerson;
		jRequestBody["report_target_name"] = m_strBadPersonName;
	}

	return jRequestBody;
}

inline bool GFLBans_Report::IsReport() const noexcept
{
	return m_jBadPerson != nullptr && !m_jBadPerson.empty();
}

void GFLBans_Report::GFLBans_CallAdmin(CCSPlayerController* pCaller)
{
	if (!pCaller)
		return;

	// Pass this into callback function, so we know if the response is for a report or calladmin query
	bool bIsReport = IsReport();

	if (std::time(nullptr) - g_pGFLBansSystem->m_wLastHeartbeat > 120)
		ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "GFLBans is currently not responding. Your %s may fail.",
					bIsReport ? "report" : "calladmin request");

	std::string strName = m_strCallerName;
	std::string strTarget = bIsReport ? m_strBadPersonName : "";
	std::string strMessage = m_strMessage;
	CHandle<CCSPlayerController> hCaller = pCaller->GetHandle();

#ifdef _DEBUG
	Message(("Report/CallAdmin Query:\n" + g_strGFLBansApiUrl + "gs/calladmin/\n").c_str());
	if (g_rghdGFLBansAuth != nullptr)
		for (HTTPHeader header : *(g_rghdGFLBansAuth))
			Message("Header - %s: %s\n", header.GetName(), header.GetValue());
	Message((CreateReportJSON().dump(1) + "\n").c_str());
#endif
	g_HTTPManager.POST(
		(g_strGFLBansApiUrl + "gs/calladmin/").c_str(),
		CreateReportJSON().dump().c_str(),
		[hCaller, bIsReport, strName, strTarget, strMessage](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
			Message(("Report/CallAdmin Response:\n" + response.dump(1) + "\n").c_str());
#endif

			CCSPlayerController* pCaller = hCaller.Get();

			if (!pCaller)
				return;

			if (!response.value("sent", false))
			{
				if (response.value("is_banned", true))
					ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "You are banned from using !report and !calladmin. Use\x0E !status\1 for more information.");
				else if (response.value("cooldown", 0) > 0)
					ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "The /%s command was used recently and is on cooldown for \2%s\1.",
								bIsReport ? "report" : "calladmin", FormatTime(response.value("cooldown", 0)).c_str());
				else
					ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "Your %s failed to send. Please try again",
								bIsReport ? "report" : "admin call");
			}
			else
			{
				for (int i = 0; i < gpGlobals->maxClients; i++)
				{
					ZEPlayer* pPlayer = g_playerManager->GetPlayer(i);

					if (pPlayer && pPlayer->IsAdminFlagSet(ADMFLAG_GENERIC))
					{
						if (bIsReport)
							ClientPrint(CCSPlayerController::FromSlot(i), HUD_PRINTTALK, GFLBANS_PREFIX "\x0F%s reported\x02 %s\x0F (reason: \x09%s\x0F).", strName.c_str(), strTarget.c_str(), strMessage.c_str());
						else
							ClientPrint(CCSPlayerController::FromSlot(i), HUD_PRINTTALK, GFLBANS_PREFIX "\x0F%s called an admin (reason: \x09%s\x0F).", strName.c_str(), strMessage.c_str());
					}
				}

				ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "Your %s was sent. If an admin is available, they will help out as soon as possible.",
							bIsReport ? "report" : "admin request");
			}
		},
		g_rghdGFLBansAuth);
}

bool GFLBansSystem::FilterMessage(CCSPlayerController* pChatter, const CCommand& args)
{
	bool bMatched = std::regex_search(args.GetCommandString(), g_regChatFilter);
	if (bMatched && g_bFilterGagDuration >= 0)
	{
		if (!pChatter)
			return true;

		ZEPlayer* zpChatter = pChatter->GetZEPlayer();

		if (zpChatter->IsFakeClient() || !zpChatter->IsAuthenticated())
			return true;

		// Comment out stacking stuff till Vauff is fine with adding it
		CreateInfraction(Gag, EchoType::Target, nullptr, pChatter, "Filtered chat message", g_bFilterGagDuration, true);
		// StackPunishment(nullptr, pChatter, "Filtered chat message", Gag, EchoType::Target, false);
	}

	// Comment out stacking stuff till Vauff is fine with adding it
	if (!bMatched)
		bMatched = std::regex_search(args.GetCommandString(), g_regChatFilterNoPunish);
	// if (!bMatched && std::regex_search(args.GetCommandString(), g_regChatFilterNoPunish))
	//{
	//	if (!pChatter)
	//		return true;

	//	ZEPlayer* zpChatter = pChatter->GetZEPlayer();

	//	if (zpChatter->IsFakeClient() || !zpChatter->IsAuthenticated())
	//		return true;

	//	bMatched = true;
	//	StackPunishment(nullptr, pChatter, "Filtered chat message", Gag, EchoType::Target, true);
	//}

	return bMatched;
}

// https://github.com/GFLClan/sm_gflbans/wiki/Implementer's-Guide-to-the-GFLBans-API#heartbeat
bool GFLBansSystem::GFLBans_Heartbeat()
{
	if (!gpGlobals || gpGlobals->maxClients < 2)
		return false;

	json jHeartbeat;

	// TODO: Properly implement when MM can read a convar's string value
	// jHeartbeat["hostname"] = g_pCVar->GetConVar(g_pCVar->FindConVar("hostname"))->GetString();
	jHeartbeat["hostname"] = g_strGFLBansHostname;

	jHeartbeat["max_slots"] = gpGlobals->maxClients;

	json jPlayers = json::array();
	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		ZEPlayer* zpPlayer = g_playerManager->GetPlayer(i);

		if (!zpPlayer || zpPlayer->IsFakeClient())
			continue;

		jPlayers.push_back(PlayerObj(zpPlayer, true));
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
	// if (g_pCVar->GetConVar(g_pCVar->FindConVar("sv_password"))->GetString().length() > 0)
	//	jHeartbeat["locked"] = true;

	// Omit for true
	if (!g_bGFLBansAcceptGlobal)
		jHeartbeat["include_other_servers"] = false;

#ifdef _DEBUG
	if (g_rghdGFLBansAuth != nullptr)
		for (HTTPHeader header : *g_rghdGFLBansAuth)
			Message("Heartbeat Header - %s: %s\n", header.GetName(), header.GetValue());
	Message(("Heartbeat Query:\nURL: " + g_strGFLBansApiUrl + "gs/heartbeat\nPOST JSON:\n" + jHeartbeat.dump(1) + "\n").c_str());
#endif

	g_HTTPManager.POST(
		(g_strGFLBansApiUrl + "gs/heartbeat").c_str(),
		jHeartbeat.dump().c_str(),
		[](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
			Message(("Heartbeat Response:\n" + response.dump(1) + "\n").c_str());
#endif
			g_pGFLBansSystem->m_wLastHeartbeat = std::time(nullptr);
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

				ZEPlayer* zpPlayer = g_playerManager->GetPlayerFromSteamId(iSteamID, true);

				if (!zpPlayer || zpPlayer->IsFakeClient())
					continue;

				CCSPlayerController* pPlayer = CCSPlayerController::FromSlot(zpPlayer->GetPlayerSlot());

				bool bWasPunished = zpPlayer->IsMuted();
				if (!g_pGFLBansSystem->CheckJSONForBlock(zpPlayer, jInfractions, Mute, true, false)
					&& bWasPunished && !zpPlayer->IsMuted())
				{
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "You are no longer %s. You may talk again.",
								GetActionPhrase(Mute, GrammarTense::Past, true));
				}

				bWasPunished = zpPlayer->IsGagged();
				if (!g_pGFLBansSystem->CheckJSONForBlock(zpPlayer, jInfractions, Gag, true, false)
					&& bWasPunished && !zpPlayer->IsGagged())
				{
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "You are no longer %s. You may type in chat again.",
								GetActionPhrase(Gag, GrammarTense::Past, true));
				}

				bWasPunished = zpPlayer->IsAdminChatGagged();
				if (!g_pGFLBansSystem->CheckJSONForBlock(zpPlayer, jInfractions, AdminChatGag, true, false)
					&& bWasPunished && !zpPlayer->IsAdminChatGagged())
				{
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "You are no longer %s. You may type in admin chat again.",
								GetActionPhrase(AdminChatGag, GrammarTense::Past, true));
				}
				// We dont need to check to check for or apply a Call Admin Block, since that is all handled by GFLBans itself

				// Ban should be checked last, since it could make zpPlayer point at garbage
				g_pGFLBansSystem->CheckJSONForBlock(zpPlayer, jInfractions, Ban);
			}
		},
		g_rghdGFLBansAuth);
	return true;
}

// https://github.com/GFLClan/sm_gflbans/wiki/Implementer's-Guide-to-the-GFLBans-API#checking-player-infractions
void GFLBansSystem::GFLBans_CheckPlayerInfractions(ZEPlayer* zpPlayer)
{
	if (zpPlayer == nullptr || zpPlayer->IsFakeClient())
		return;

	// Check against current infractions on the server and remove expired ones
	g_pAdminSystem->ApplyInfractions(zpPlayer);

	// We dont care if zpPlayer is authenticated or not at this point.
	// We are just fetching active punishments rather than applying anything new, so innocents
	// cannot be hurt by someone faking a steamid here
	std::string strURL = PlayerQuery(zpPlayer, true);
	if (strURL.length() == 0)
		return;
	ZEPlayerHandle zphPlayer = zpPlayer->GetHandle();

	g_HTTPManager.GET(strURL.c_str(), [zphPlayer](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
		Message(("Check Infraction Response:\n" + response.dump(1) + "\n").c_str());
#endif
		ZEPlayer* zpPlayer = zphPlayer.Get();
		if (!zpPlayer)
			return;

		g_pGFLBansSystem->CheckJSONForBlock(zpPlayer, response, Mute, true, false);
		g_pGFLBansSystem->CheckJSONForBlock(zpPlayer, response, Gag, true, false);
		g_pGFLBansSystem->CheckJSONForBlock(zpPlayer, response, AdminChatGag, true, false);
		// We dont need to check to apply a Call Admin Block server side, since that is all handled by GFLBans itself

		// Ban should be checked last, since it could make zpPlayer point at garbage
		g_pGFLBansSystem->CheckJSONForBlock(zpPlayer, response, Ban, true, false); }, g_rghdGFLBansAuth);
}

// https://github.com/GFLClan/sm_gflbans/wiki/Implementer's-Guide-to-the-GFLBans-API#old-style-infractions
void GFLBansSystem::GFLBans_CreateInfraction(std::shared_ptr<GFLBans_Infraction> infPunishment,
											 CCSPlayerController* pBadPerson, CCSPlayerController* pAdmin,
											 EchoType echo)
{
	if (!pBadPerson)
		return;

	ZEPlayer* plyBadPerson = pBadPerson->GetZEPlayer();
	if (!plyBadPerson || plyBadPerson->IsFakeClient())
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not on the server...");
		return;
	}
	else if (!plyBadPerson->IsAuthenticated())
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not authenticated, please wait a bit and try again later.");
		return;
	}

	if ((infPunishment->IsSession() && infPunishment->GetInfractionType() != Ban) || std::time(nullptr) - m_wLastHeartbeat > 120)
	{
		// Make sure session punishments are applied. It would be nice to log them, but if GFLBans
		// is down, we still want these to stick. Also, if last 2 heartbeats failed, assume
		// GFLBans is down and simply apply a map-length punishment as well to make sure it sticks
		// until GFLBans is back up.
		CInfractionBase* infraction;
		switch (infPunishment->GetInfractionType())
		{
			case Mute:
				infraction = new CMuteInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case Gag:
				infraction = new CGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case Ban:
				infraction = new CBanInfraction(0, plyBadPerson->GetSteamId64());
				break;
			case Silence:
				infraction = new CMuteInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				g_pAdminSystem->AddInfraction(infraction);
				infraction->ApplyInfraction(plyBadPerson);
				infraction = new CGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case AdminChatGag:
				infraction = new CAdminChatGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case CallAdminBlock:
			case Warn:
				infraction = nullptr;
				break;
			default:
				// This should never be reached, since we it means we are trying to apply an unimplemented block type
				ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Improper block type... Send to a dev with the command used.");
				return;
		}

		std::string strBadPlyName = pBadPerson->GetPlayerName();
		std::string strPunishment = std::string(GetActionPhrase(infPunishment->GetInfractionType(), GrammarTense::Past, true)) + " " + strBadPlyName + " until the map changes";

		if (infPunishment->GetReason() != "No reason provided")
			strPunishment.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");

		if (std::time(nullptr) - m_wLastHeartbeat > 120)
			strPunishment.append(" (\2GFLBans is currently not responding\1)");

		EchoMessage(pAdmin, pBadPerson, strPunishment.c_str(), echo);

		if (infraction)
		{
			// We're overwriting the infraction, so remove the previous one first
			g_pAdminSystem->AddInfraction(infraction);
			infraction->ApplyInfraction(plyBadPerson);
		}
	}

	CHandle<CCSPlayerController> hBadPerson = pBadPerson->GetHandle();
	CHandle<CCSPlayerController> hAdmin = pAdmin ? pAdmin->GetHandle() : nullptr;

#ifdef _DEBUG
	Message(("Create Infraction Query:\n" + g_strGFLBansApiUrl + "infractions/\n").c_str());
	if (g_rghdGFLBansAuth != nullptr)
		for (HTTPHeader header : *g_rghdGFLBansAuth)
			Message("Header - %s: %s\n", header.GetName(), header.GetValue());
	Message((infPunishment->CreateInfractionJSON().dump(1) + "\n").c_str());
#endif

	g_HTTPManager.POST(
		(g_strGFLBansApiUrl + "infractions/").c_str(),
		infPunishment->CreateInfractionJSON().dump().c_str(),
		[infPunishment, hBadPerson, hAdmin, echo](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
			Message(("Create Infraction Response:\n" + response.dump(1) + "\n").c_str());
#endif

			if (infPunishment->IsSession())
				// Session punishments don't care about response, since GFLBans instantly expires them
				return;

			CCSPlayerController* pBadPerson = hBadPerson.Get();
			CCSPlayerController* pAdmin = hAdmin ? hAdmin.Get() : nullptr;

			if (!pBadPerson)
				return;

			ZEPlayer* plyBadPerson = pBadPerson->GetZEPlayer();
			if (!plyBadPerson || plyBadPerson->IsFakeClient() || !plyBadPerson->IsAuthenticated())
			{
				// This should only be hit if the player disconnected in the time between the query
				// being sent and GFLBans responding to the query. Punishment is logged, but nothing to apply.
				ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player has left the server. Punishment has been logged on GFLBans.");
				return;
			}

			std::time_t iDuration = response.value("time_left", 0);
			if (iDuration == 0)
			{
				iDuration = response.value("expires", 0);
				if (iDuration != 0)
					iDuration -= response.value("created", 0);
			}
			iDuration = static_cast<int>(std::ceil(iDuration / 60.0)); // Convert from seconds to minutes

			CInfractionBase* infraction;
			switch (infPunishment->GetInfractionType())
			{
				case Mute:
					infraction = new CMuteInfraction(iDuration, plyBadPerson->GetSteamId64());
					break;
				case Gag:
					infraction = new CGagInfraction(iDuration, plyBadPerson->GetSteamId64());
					break;
				case Ban:
					infraction = new CBanInfraction(iDuration, plyBadPerson->GetSteamId64());
					break;
				case Silence:
					infraction = new CMuteInfraction(0, plyBadPerson->GetSteamId64());
					g_pAdminSystem->FindAndRemoveInfraction(plyBadPerson, infraction->GetType(), false);
					g_pAdminSystem->AddInfraction(infraction);
					infraction->ApplyInfraction(plyBadPerson);
					infraction = new CGagInfraction(0, plyBadPerson->GetSteamId64());
					break;
				case AdminChatGag:
					infraction = new CAdminChatGagInfraction(0, plyBadPerson->GetSteamId64());
					break;
				case CallAdminBlock:
				case Warn:
					infraction = nullptr;
					break;
				default:
					// This should never be reached, since we it means we are trying to apply an unimplemented block type
					ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Improper block type... Send to a dev with the command used.");
					return;
			}
			std::string strPunishment = GetActionPhrase(infPunishment->GetInfractionType(), GrammarTense::Past, true);
			std::string strBadPlyName = pBadPerson->GetPlayerName();

			if (iDuration == 0)
				strPunishment = "\2permanently\1 " + strPunishment + " " + strBadPlyName;
			else
				strPunishment.append(" " + strBadPlyName + " for \2" + FormatTime(iDuration, false) + "\1");

			if (infPunishment->GetReason() != "No reason provided")
				strPunishment.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");

			EchoMessage(pAdmin, pBadPerson, strPunishment.c_str(), echo);

			if (infraction)
			{
				// We're overwriting the infraction, so remove the previous one first
				g_pAdminSystem->FindAndRemoveInfraction(plyBadPerson, infraction->GetType(), false);
				g_pAdminSystem->AddInfraction(infraction);
				infraction->ApplyInfraction(plyBadPerson);
			}
		},
		g_rghdGFLBansAuth);
}

// Send a POST request to GFLBans telling it to remove all blocks of a given type for a player
// https://github.com/GFLClan/sm_gflbans/wiki/Implementer's-Guide-to-the-GFLBans-API#removing-infractions
void GFLBansSystem::GFLBans_RemoveInfraction(std::shared_ptr<GFLBans_InfractionRemoval> infPunishment,
											 CCSPlayerController* pGoodPerson, CCSPlayerController* pAdmin,
											 EchoType echo)
{
	if (!pGoodPerson)
		return;

	ZEPlayer* zpGoodPerson = pGoodPerson->GetZEPlayer();
	if (!zpGoodPerson || zpGoodPerson->IsFakeClient())
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not on the server...");
		return;
	}
	else if (!zpGoodPerson->IsAuthenticated())
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not authenticated, please wait a bit and try again later.");
		return;
	}

	if (std::time(nullptr) - m_wLastHeartbeat > 120)
	{
		// GFLBans has not responded for 2+ heartbeats. Remove punishment on server, but it will be
		//  automatically reapplied when GFLBans comes back up if not removed on the web
		bool bIsPunished = false;
		switch (infPunishment->GetInfractionType())
		{
			case Mute:
				bIsPunished = zpGoodPerson->IsMuted();
				g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::Mute, false);
				break;
			case Gag:
				bIsPunished = zpGoodPerson->IsGagged();
				g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::Gag, false);
				break;
			case Silence:
				bIsPunished = zpGoodPerson->IsGagged() || zpGoodPerson->IsMuted();
				g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::Mute, false);
				g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::Gag, false);
				break;
			case AdminChatGag:
				bIsPunished = zpGoodPerson->IsAdminChatGagged();
				g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::AdminChatGag, false);
				break;
		}
		if (bIsPunished)
			ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Local block removed, but \2GFLBans is currently down\1. Any web blocks will be reapplied when GFLBans comes back online.");
	}

	CHandle<CCSPlayerController> hGoodPerson = pGoodPerson->GetHandle();
	CHandle<CCSPlayerController> hAdmin = pAdmin ? pAdmin->GetHandle() : nullptr;

#ifdef _DEBUG
	Message(("Remove Infraction Query:\n" + g_strGFLBansApiUrl + "infractions/remove\n").c_str());
	if (g_rghdGFLBansAuth != nullptr)
		for (HTTPHeader header : *g_rghdGFLBansAuth)
			Message("Header - %s: %s\n", header.GetName(), header.GetValue());
	Message((infPunishment->CreateInfractionJSON().dump(1) + "\n").c_str());
#endif

	g_HTTPManager.POST(
		(g_strGFLBansApiUrl + "infractions/remove").c_str(),
		infPunishment->CreateInfractionJSON().dump().c_str(),
		[infPunishment, hGoodPerson, hAdmin, echo](HTTPRequestHandle request, json response) {

#ifdef _DEBUG
			Message(("Remove Infraction Response:\n" + response.dump(1) + "\n").c_str());
#endif

			CCSPlayerController* pGoodPerson = hGoodPerson.Get();
			CCSPlayerController* pAdmin = hAdmin != nullptr ? hAdmin.Get() : nullptr;

			if (!pGoodPerson)
				return;

			ZEPlayer* zpGoodPerson = pGoodPerson->GetZEPlayer();
			if (!zpGoodPerson || zpGoodPerson->IsFakeClient())
			{
				// This should only be hit if the player disconnected in the time between the query being sent
				// and GFLBans responding to the query
				ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not currently on the server. Any blocks have been removed from GFLBans.");
				return;
			}
			// Invalidate local punishments of infraction's type
			CInfractionBase::EInfractionType itypeToRemove;
			bool bRemoveGagAndMute = false;
			bool bIsPunished = false;
			switch (infPunishment->GetInfractionType())
			{
				case Mute:
					bIsPunished = zpGoodPerson->IsMuted();
					itypeToRemove = CInfractionBase::EInfractionType::Mute;
					break;
				case Gag:
					bIsPunished = zpGoodPerson->IsGagged();
					itypeToRemove = CInfractionBase::EInfractionType::Gag;
					break;
				case Ban:
					// This should never be hit, since zpGoodPerson wouldn't be valid (connected) if they were banned
					itypeToRemove = CInfractionBase::EInfractionType::Ban;
					break;
				case Silence:
					bIsPunished = zpGoodPerson->IsGagged() || zpGoodPerson->IsMuted();
					itypeToRemove = CInfractionBase::EInfractionType::Mute;
					bRemoveGagAndMute = true;
					break;
				case AdminChatGag:
					bIsPunished = zpGoodPerson->IsAdminChatGagged();
					itypeToRemove = CInfractionBase::EInfractionType::AdminChatGag;
					break;
				case CallAdminBlock:
				case Warn:
					bIsPunished = true; // We dont store these locally, so just assume they were punished and now aren't. (/^.^)/
					break;
				default:
					// This should never be reached, since we it means we are trying to apply an unimplemented block type
					ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Improper block type... Send to a dev with the command used.");
					return;
			}

			if (!bIsPunished)
			{
				ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "%s is not %s.", pGoodPerson->GetPlayerName(),
							GetActionPhrase(infPunishment->GetInfractionType(), GrammarTense::Past, true));
				return;
			}

			std::string strAction = GetActionPhrase(infPunishment->GetInfractionType(), GrammarTense::Past, false);
			strAction.append(" ");
			strAction.append(pGoodPerson->GetPlayerName());
			if (infPunishment->GetReason() != "No reason provided")
				strAction.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");
			EchoMessage(pAdmin, pGoodPerson, strAction.c_str(), echo);

			if (infPunishment->GetInfractionType() != CallAdminBlock)
				g_pAdminSystem->RemoveInfractionType(zpGoodPerson, itypeToRemove, bRemoveGagAndMute);
		},
		g_rghdGFLBansAuth);
}

bool GFLBansSystem::CheckJSONForBlock(ZEPlayer* zpPlayer, json jAllBlockInfo, InfType blockType,
									  bool bApplyBlock, bool bRemoveSession)
{
	if (!zpPlayer || zpPlayer->IsFakeClient())
		return false;

	std::string strBlockType;

	switch (blockType)
	{
		case Mute:
			strBlockType = "voice_block";
			break;
		case Gag:
			strBlockType = "chat_block";
			break;
		case Ban:
			strBlockType = "ban";
			break;
		case AdminChatGag:
			strBlockType = "admin_chat_block";
			break;
		default:
			// We dont need to check to apply a Call Admin Block server side, since that is all handled by GFLBans itself
			return false;
	}

	json jBlockInfo = jAllBlockInfo.value(strBlockType, json());
	if (jBlockInfo.empty())
	{
		if (zpPlayer->IsAuthenticated())
		{
			switch (blockType)
			{
				case Mute:
					g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, CInfractionBase::EInfractionType::Mute, bRemoveSession);
					break;
				case Gag:
					g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, CInfractionBase::EInfractionType::Gag, bRemoveSession);
					break;
				case Ban:
					g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, CInfractionBase::EInfractionType::Ban, bRemoveSession);
					break;
				case AdminChatGag:
					g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, CInfractionBase::EInfractionType::AdminChatGag, bRemoveSession);
					break;
			}
		}
		return false;
	}

	if (bApplyBlock)
	{
		time_t iDuration = jBlockInfo.value("expiration", std::time(nullptr)) - std::time(nullptr);
		iDuration = static_cast<time_t>(std::ceil(iDuration / 60.0));

		uint64 iSteamID = zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64();

		CInfractionBase* infraction;
		switch (blockType)
		{
			case Mute:
				infraction = new CMuteInfraction(iDuration, iSteamID);
				break;
			case Gag:
				infraction = new CGagInfraction(iDuration, iSteamID);
				break;
			case Ban:
				infraction = new CBanInfraction(iDuration, iSteamID);
				break;
			case AdminChatGag:
				infraction = new CAdminChatGagInfraction(iDuration, iSteamID);
				break;
			default:
				return true;
		}

		// Overwrite any existing infractions of the same type and update from web
		// This is in case a current infraction was edited on the web
		g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, infraction->GetType(), bRemoveSession);
		g_pAdminSystem->AddInfraction(infraction);
		infraction->ApplyInfraction(zpPlayer);
	}
	return true;
}

void GFLBansSystem::CheckPunishmentHistory(CCSPlayerController* pAdmin, CCSPlayerController* pBadPerson,
										   std::string strReason)
{
	auto vecInfractions = std::make_shared<std::vector<int>>(Warn + 1, -1);
	return GetPunishmentHistory(vecInfractions, 0, pAdmin ? pAdmin->GetHandle() : nullptr, pBadPerson->GetHandle(), strReason);
}

void GFLBansSystem::GetPunishmentHistory(std::shared_ptr<std::vector<int>> vecInfractions, int iCounted,
										 CHandle<CCSPlayerController> hAdmin, CHandle<CCSPlayerController> hBadPerson,
										 std::string strReason)
{
	CCSPlayerController* pBadPerson = hBadPerson.Get();
	if (!pBadPerson)
		return;

	ZEPlayer* zpBadPerson = pBadPerson->GetZEPlayer();
	if (!zpBadPerson || zpBadPerson->IsFakeClient() || !zpBadPerson->IsAuthenticated())
		return;

	std::string strURL = g_strGFLBansApiUrl + "infractions/?gs_service=steam&gs_id=" + std::to_string(zpBadPerson->GetSteamId64());
	std::string strIP = zpBadPerson->GetIpAddress();
	if (strIP.length() > 0 && strIP != "127.0.0.1")
		strURL.append("&ip=" + strIP);

	// Max infractions GFLBans will respond with is 50, so use recursion to count everything
	strURL = strURL + "&limit=50&skip=" + std::to_string(iCounted);
	iCounted += 50;

	g_HTTPManager.GET(
		strURL.c_str(),
		[vecInfractions, iCounted, hAdmin, hBadPerson, strReason](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
			Message("GetPunishmentHistory response: %s\n", response.dump().c_str());
#endif
			json jInfractions = response.value("results", json::array());
			int iTotalInfractions = response.value("total_matched", 0);
			std::string strReasonLower = strReason;
			std::transform(strReasonLower.begin(), strReasonLower.end(), strReasonLower.begin(),
						   [](unsigned char c) { return std::tolower(c); });

			for (int i = 0; i < jInfractions.size(); i++)
			{
				uint64 iFlags = jInfractions[i].value("flags", 0);

				// Don't stack with removed punishments
				if (iFlags & InfractionFlags::REMOVED)
					continue;

				std::string strTemp = jInfractions[i].value("reason", "");
				std::transform(strTemp.begin(), strTemp.end(), strTemp.begin(), [](unsigned char c) { return std::tolower(c); });
				// If strReason.length() is 0, check all reasons. Otherewise only check if reason contains strReason
				if (strReason.length() > 0 && strTemp.find(strReasonLower) == std::string::npos)
					continue;

				int iDuration = -1;

				if (iFlags & InfractionFlags::PERMANENT)
					iDuration = 0;
				else if (iFlags & InfractionFlags::SESSION)
					iDuration = 30 * 60;
				else if (jInfractions[i].value("orig_length", -1) > 0)
					iDuration = static_cast<int>(jInfractions[i].value("orig_length", -1));
				else if (jInfractions[i].value("expires", -1) > 0 && jInfractions[i].value("created", -1) > 0)
					iDuration = static_cast<int>(jInfractions[i].value("expires", -1) - jInfractions[i].value("created", -1));

				if (iDuration < 0)
					continue;
				if (iDuration > 31536000) // 1 year+, just call it perma
					iDuration = 0;

				if (!(iFlags & NOT_WARNING))
				{
					if (vecInfractions->at(Warn) != 0 && (iDuration == 0 || iDuration > vecInfractions->at(Warn)))
						(*vecInfractions)[Warn] = iDuration;
					continue;
				}

				// Log the punishment if it is greater than last max
				if (iFlags & InfractionFlags::BAN && vecInfractions->at(Ban) != 0 && (iDuration == 0 || iDuration > vecInfractions->at(Ban)))
					(*vecInfractions)[Ban] = iDuration;
				if (iFlags & InfractionFlags::VOICE_BLOCK && vecInfractions->at(Mute) != 0 && (iDuration == 0 || iDuration > vecInfractions->at(Mute)))
					(*vecInfractions)[Mute] = iDuration;
				if (iFlags & InfractionFlags::CHAT_BLOCK && vecInfractions->at(Gag) != 0 && (iDuration == 0 || iDuration > vecInfractions->at(Gag)))
					(*vecInfractions)[Gag] = iDuration;
				if (iFlags & InfractionFlags::ADMIN_CHAT_BLOCK && vecInfractions->at(AdminChatGag) != 0 && (iDuration == 0 || iDuration > vecInfractions->at(AdminChatGag)))
					(*vecInfractions)[AdminChatGag] = iDuration;
				if (iFlags & InfractionFlags::CALL_ADMIN_BAN && vecInfractions->at(CallAdminBlock) != 0 && (iDuration == 0 || iDuration > vecInfractions->at(CallAdminBlock)))
					(*vecInfractions)[CallAdminBlock] = iDuration;
			}

			if (iCounted >= iTotalInfractions)
				g_pGFLBansSystem->DisplayPunishmentHistory(vecInfractions, hAdmin, hBadPerson, strReason);
			else
				g_pGFLBansSystem->GetPunishmentHistory(vecInfractions, iCounted, hAdmin, hBadPerson, strReason);
		},
		g_rghdGFLBansAuth);

	return;
}

// Use info from GetPunishmentStacks to actually perform an action
void GFLBansSystem::DisplayPunishmentHistory(std::shared_ptr<std::vector<int>> vecInfractions,
											 CHandle<CCSPlayerController> hAdmin, CHandle<CCSPlayerController> hBadPerson,
											 std::string strReason)
{
	CCSPlayerController* pAdmin = hAdmin ? hAdmin.Get() : nullptr;
	CCSPlayerController* pBadPerson = hBadPerson.Get();
	if (!pBadPerson)
	{
		if (strReason.length() == 0)
			ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Target not found.");
		return;
	}

	bool bHasInfraction = false;
	for (int i = 0; i < vecInfractions->size(); i++)
	{
		if (vecInfractions->at(i) >= 0)
		{
			bHasInfraction = true;
			break;
		}
	}

	if (!bHasInfraction)
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "%s has no infraction history.", pBadPerson->GetPlayerName());
		return;
	}

	ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Check console for %s's punishment history.", pBadPerson->GetPlayerName());
	ClientPrint(pAdmin, HUD_PRINTCONSOLE, "%s's longest infractions:", pBadPerson->GetPlayerName());
	if (vecInfractions->at(Ban) == 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tBan: Permanent");
	else if (vecInfractions->at(Ban) > 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tBan: %s", FormatTime(vecInfractions->at(Ban), true).c_str());

	if (vecInfractions->at(Mute) == 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tMute: Permanent");
	else if (vecInfractions->at(Mute) > 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tMute: %s", FormatTime(vecInfractions->at(Mute), true).c_str());

	if (vecInfractions->at(Gag) == 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tGag: Permanent");
	else if (vecInfractions->at(Gag) > 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tGag: %s", FormatTime(vecInfractions->at(Gag), true).c_str());

	if (vecInfractions->at(AdminChatGag) == 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tAdmin Chat Gag: Permanent");
	else if (vecInfractions->at(AdminChatGag) > 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tAdmin Chat Gag: %s", FormatTime(vecInfractions->at(AdminChatGag), true).c_str());

	if (vecInfractions->at(CallAdminBlock) == 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tCall Admin Block: Permanent");
	else if (vecInfractions->at(CallAdminBlock) > 0)
		ClientPrint(pAdmin, HUD_PRINTCONSOLE, "\tCall Admin Block: %s", FormatTime(vecInfractions->at(CallAdminBlock), true).c_str());
}

bool GFLBansSystem::StackPunishment(CCSPlayerController* pAdmin, CCSPlayerController* pBadPerson,
									std::string strReason, InfType iType, EchoType echo, bool bWarnFirst)
{
	auto vecInfractions = std::make_shared<std::vector<int>>(Warn + 1, -1);
	return GetPunishmentStacks(vecInfractions, 0, pAdmin ? pAdmin->GetHandle() : nullptr, pBadPerson->GetHandle(), strReason, iType, echo, bWarnFirst);
}

bool GFLBansSystem::GetPunishmentStacks(std::shared_ptr<std::vector<int>> vecInfractions, int iCounted,
										CHandle<CCSPlayerController> hAdmin, CHandle<CCSPlayerController> hBadPerson,
										std::string strReason, InfType iType, EchoType echo, bool bWarnFirst)
{
	CCSPlayerController* pBadPerson = hBadPerson.Get();
	if (!pBadPerson)
		return false;

	ZEPlayer* zpBadPerson = pBadPerson->GetZEPlayer();
	if (!zpBadPerson || zpBadPerson->IsFakeClient() || !zpBadPerson->IsAuthenticated())
		return false;

	std::string strURL = g_strGFLBansApiUrl + "infractions/?gs_service=steam&gs_id=" + std::to_string(zpBadPerson->GetSteamId64());
	std::string strIP = zpBadPerson->GetIpAddress();
	if (strIP.length() > 0 && strIP != "127.0.0.1")
		strURL.append("&ip=" + strIP);

	// Max infractions GFLBans will respond with is 50, so use recursion to count everything
	strURL = strURL + "&limit=50&skip=" + std::to_string(iCounted);
	iCounted += 50;

	g_HTTPManager.GET(
		strURL.c_str(),
		[vecInfractions, iCounted, hAdmin, hBadPerson, strReason, iType, echo, bWarnFirst](HTTPRequestHandle request, json response) {
#ifdef _DEBUG
			Message("GetPunishmentStacks response: %s\n", response.dump().c_str());
#endif
			json jInfractions = response.value("results", json::array());
			int iTotalInfractions = response.value("total_matched", 0);
			std::string strReasonLower = strReason;
			std::transform(strReasonLower.begin(), strReasonLower.end(), strReasonLower.begin(),
						   [](unsigned char c) { return std::tolower(c); });

			for (int i = 0; i < jInfractions.size(); i++)
			{
				uint64 iFlags = jInfractions[i].value("flags", 0);
				bool bLoggingWarning = bWarnFirst && ((iFlags & InfractionFlags::NOT_WARNING) == 0);

				switch (iType)
				{
					case Ban:
						if (!(iFlags & InfractionFlags::BAN) && !bLoggingWarning)
							continue;
						break;
					case Mute:
						if (!(iFlags & InfractionFlags::VOICE_BLOCK) && !bLoggingWarning)
							continue;
						break;
					case Gag:
						if (!(iFlags & InfractionFlags::CHAT_BLOCK) && !bLoggingWarning)
							continue;
						break;
					case Silence:
						if (!(iFlags & (InfractionFlags::VOICE_BLOCK | InfractionFlags::CHAT_BLOCK)) && !bLoggingWarning)
							continue;
						break;
					case AdminChatGag:
						if (!(iFlags & InfractionFlags::ADMIN_CHAT_BLOCK) && !bLoggingWarning)
							continue;
						break;
					case CallAdminBlock:
						if (!(iFlags & InfractionFlags::CALL_ADMIN_BAN) && !bLoggingWarning)
							continue;
						break;
				}

				// Don't stack with removed punishments
				if (iFlags & InfractionFlags::REMOVED)
					continue;

				std::string strTemp = jInfractions[i].value("reason", "");
				std::transform(strTemp.begin(), strTemp.end(), strTemp.begin(), [](unsigned char c) { return std::tolower(c); });
				// If strReason.length() is 0, check all reasons. Otherewise only check if reason contains strReason
				if (strReason.length() > 0 && strTemp.find(strReasonLower) == std::string::npos)
					continue;

				int iDuration = -1;

				if (iFlags & InfractionFlags::PERMANENT)
					iDuration = 0;
				else if (iFlags & InfractionFlags::SESSION)
					iDuration = 30 * 60;
				else if (jInfractions[i].value("orig_length", -1) > 0)
					iDuration = static_cast<int>(jInfractions[i].value("orig_length", -1));
				else if (jInfractions[i].value("expires", -1) > 0 && jInfractions[i].value("created", -1) > 0)
					iDuration = static_cast<int>(jInfractions[i].value("expires", -1) - jInfractions[i].value("created", -1));

				if (iDuration < 0)
					continue;
				if (iDuration > 31536000) // 1 year+, just call it perma
					iDuration = 0;

				if (bLoggingWarning)
					(*vecInfractions)[Warn] = iDuration;
				else
					(*vecInfractions)[iType] = iDuration;

				if ((*vecInfractions)[iType] == 0)
					break;
			}

			if (iCounted >= iTotalInfractions || vecInfractions->at(iType) == 0)
				g_pGFLBansSystem->ApplyStackedPunishment(vecInfractions, hAdmin, hBadPerson, strReason, iType, echo, bWarnFirst);
			else
				g_pGFLBansSystem->GetPunishmentStacks(vecInfractions, iCounted, hAdmin, hBadPerson, strReason, iType, echo, bWarnFirst);
		},
		g_rghdGFLBansAuth);

	return true;
}

// Use info from GetPunishmentStacks to actually perform an action
void GFLBansSystem::ApplyStackedPunishment(std::shared_ptr<std::vector<int>> vecInfractions,
										   CHandle<CCSPlayerController> hAdmin, CHandle<CCSPlayerController> hBadPerson,
										   std::string strReason, InfType iType, EchoType echo, bool bWarnFirst)
{
	CCSPlayerController* pAdmin = hAdmin ? hAdmin.Get() : nullptr;
	CCSPlayerController* pBadPerson = hBadPerson.Get();
	if (!pBadPerson)
	{
		if (strReason.length() == 0 || echo != None)
			ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Target not found.");
		return;
	}

	int iDuration = vecInfractions->at(iType) * 2;
	if (iDuration < 0 && vecInfractions->at(Warn) < 0 && bWarnFirst)
	{
		// Player has no past gags or active warnings for this. Give them a 1 week Warn and let them off without a punishment.
		CreateInfraction(Warn, echo, pAdmin, pBadPerson, strReason, 0, true);
		return;
	}
	else if (iDuration < 0)
		iDuration = g_bFilterGagDuration;
	else if (iDuration > 31536000) // 1 year, just set perma
		iDuration = 0;

	CreateInfraction(iType, echo, pAdmin, pBadPerson, strReason, iDuration / 60, true);
}

// --- Helper functions ---
void CreateInfraction(InfType infType, EchoType echo, CCSPlayerController* pAdmin,
					  CCSPlayerController* pBadPerson, std::string strReason, int iDuration,
					  bool bOnlineOnly)
{
	if (!pBadPerson)
		return;

	ZEPlayer* zpBadPerson = pBadPerson->GetZEPlayer();

	if (zpBadPerson->IsFakeClient() || !zpBadPerson->IsAuthenticated())
		return;

	auto gflInfraction = std::make_shared<GFLBans_Infraction>(infType, pBadPerson->GetHandle(), strReason,
															  pAdmin ? pAdmin->GetHandle() : nullptr,
															  iDuration, bOnlineOnly);

	g_pGFLBansSystem->GFLBans_CreateInfraction(gflInfraction, pBadPerson, pAdmin, echo);
}

void RemoveInfraction(InfType infType, EchoType echo, CCSPlayerController* pAdmin,
					  CCSPlayerController* pGoodPerson, std::string strReason)
{
	if (!pGoodPerson)
		return;

	ZEPlayer* zpGoodPerson = pGoodPerson->GetZEPlayer();

	if (zpGoodPerson->IsFakeClient() || !zpGoodPerson->IsAuthenticated())
		return;

	auto gflInfraction = std::make_shared<GFLBans_InfractionRemoval>(infType, pGoodPerson->GetHandle(),
																	 strReason, pAdmin ? pAdmin->GetHandle() : nullptr);

	g_pGFLBansSystem->GFLBans_RemoveInfraction(gflInfraction, pGoodPerson, pAdmin, echo);
}

// Parses a chat command to add or remove an infType of punishment
void ParseInfraction(const CCommand& args, CCSPlayerController* pAdmin, bool bAdding, InfType infType)
{
	if (args.ArgC() < 2 || (bAdding && infType == Ban && args.ArgC() < 3))
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Usage: !%s <name> %s[reason]",
					GetActionPhrase(infType, PresentOrNoun, bAdding), bAdding ? "[duration] " : "");
		return;
	}

	int iDuration = bAdding ? ParseTimeInput(args[2]) : 0;
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];
	ETargetType nType;

	uint64 iBlockedFlags = NO_RANDOM | NO_BOT | NO_UNAUTHENTICATED;
	if (bAdding)
		iBlockedFlags |= NO_SELF;

	// Only allow multiple targetting for mutes that aren't perma (ie. !mute @all 1) for stopping mass mic spam
	if (infType != Mute || (bAdding && iDuration == 0))
		iBlockedFlags |= NO_MULTIPLE;

	ETargetError eType = g_playerManager->GetPlayersFromString(pAdmin, args[1], iNumClients, pSlots, iBlockedFlags, nType);

	if (bAdding && iDuration == 0 && (eType == ETargetError::MULTIPLE || eType == ETargetError::RANDOM))
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "You may only permanently %s individuals.",
					GetActionPhrase(infType, GrammarTense::PresentOrNoun, bAdding));
		return;
	}
	else if (eType != ETargetError::NO_ERRORS)
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "%s", g_playerManager->GetErrorString(eType, (iNumClients == 0) ? 0 : pSlots[0]).c_str());
		return;
	}

	const char* pszCommandPlayerName = pAdmin ? pAdmin->GetPlayerName() : CONSOLE_NAME;

	// Dont allow session punishments for bans (since we don't want to allow admins map-banning players)
	// or for CallAdminBlocks (since these are handled by GFLBans, not the game server)
	if (bAdding && iDuration < 0 && (infType == Ban || infType == CallAdminBlock))
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Invalid duration.");
		return;
	}

	std::string strReason = GetReason(args, bAdding ? 2 : 1, true);

	if (iNumClients > 1 && infType == Mute)
	{
		// Targetting a group of people. Do not log these on GFLBans.
		for (int i = 0; i < iNumClients; i++)
		{
			CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

			if (!pTarget)
				continue;

			ZEPlayer* zpTarget = g_playerManager->GetPlayer(pSlots[i]);

			if (zpTarget->IsFakeClient())
				continue;

			if (!bAdding)
			{
				if (g_pAdminSystem->FindAndRemoveInfraction(zpTarget, CInfractionBase::Mute))
					// Prevent players with web mutes from speaking after a mass ummute
					g_pGFLBansSystem->GFLBans_CheckPlayerInfractions(zpTarget);
			}
			else
			{
				// We only allow mass muting for mutes, so don't need to set infraction type here
				CInfractionBase* infraction = new CMuteInfraction(iDuration < 0 ? 0 : iDuration,
																  zpTarget->GetSteamId64(),
																  false, true);

				// We're overwriting the infraction, so remove the previous one first
				g_pAdminSystem->FindAndRemoveInfraction(zpTarget, CInfractionBase::Mute);
				g_pAdminSystem->AddInfraction(infraction);
				infraction->ApplyInfraction(zpTarget);
				if (iDuration > 0)
					// Only run this once since we simply dont need to heartbeat it. We know when the punishment ends
					g_pAdminSystem->RemoveSessionPunishments(iDuration * 60.0 + 1);
			}
		}

		const char* pszCommandPlayerName = pAdmin ? pAdmin->GetPlayerName() : CONSOLE_NAME;
		if (bAdding)
		{
			char szAction[64];
			V_snprintf(szAction, sizeof(szAction), " for %s", FormatTime(iDuration, false).c_str());
			PrintMultiAdminAction(nType, pszCommandPlayerName, "muted", szAction, GFLBANS_PREFIX);
		}
		else
			PrintMultiAdminAction(nType, pszCommandPlayerName, "unmuted", "", GFLBANS_PREFIX);

		return;
	}

	// We should be targetting only a single player from this point on
	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[0]);
	ZEPlayer* zpTarget = pTarget->GetZEPlayer();

	if (bAdding)
	{
		bool bOnlineOnly = iDuration > 0 && (g_iMinRealWorldDuration <= 0 || args[2][0] == '+' || iDuration < g_iMinRealWorldDuration);
		CreateInfraction(infType, EchoType::All, pAdmin, pTarget, strReason, iDuration, bOnlineOnly);
	}
	else
		RemoveInfraction(infType, EchoType::All, pAdmin, pTarget, strReason);
}

// Prints out a formatted list of punishments to the player's console.
// Does not include session punishments, as GFLBans will not return those.
void ConsoleListPunishments(CCSPlayerController* const player, json punishments)
{
	for (const auto& punishment : punishments.items())
	{
		time_t wExpiration = punishment.value().value("expiration", -1);
		std::string strPunishmentType;

		if (punishment.key() == "voice_block")
			strPunishmentType = GetActionPhrase(Mute, GrammarTense::Past, true);
		else if (punishment.key() == "chat_block")
			strPunishmentType = GetActionPhrase(Gag, GrammarTense::Past, true);
		else if (punishment.key() == "ban")
			strPunishmentType = GetActionPhrase(Ban, GrammarTense::Past, true);
		else if (punishment.key() == "admin_chat_block")
			strPunishmentType = GetActionPhrase(AdminChatGag, GrammarTense::Past, true);
		else if (punishment.key() == "call_admin_block")
			strPunishmentType = GetActionPhrase(CallAdminBlock, GrammarTense::Past, true);
		else
			strPunishmentType = punishment.key();

		if (wExpiration <= 0)
			strPunishmentType = "Permanently " + strPunishmentType + ":";
		else
		{
			strPunishmentType.at(0) = std::toupper(strPunishmentType.at(0));
			strPunishmentType.append(" for " + FormatTime(wExpiration - std::time(nullptr)) + ":");
		}

		ClientPrint(player, HUD_PRINTCONSOLE, strPunishmentType.c_str());

		for (const auto& val : punishment.value().items())
		{
			std::string desc;
			if (val.key() == "expiration")
				continue;
			else if (val.key() == "admin_name")
				desc = "\tAdmin: ";
			else if (val.key() == "reason")
			{
				if (val.value().dump() == "\"No reason provided\"")
					continue;
				desc = "\tReason: ";
			}
			else
				desc = "\t" + val.key() + ": ";
			std::string temp = val.value().dump();
			desc.append(temp.substr(1, temp.length() - 2));
			ClientPrint(player, HUD_PRINTCONSOLE, desc.c_str());
		}
	}
}

// Returns a string matching the type of punishment and grammar tense specified
const char* GetActionPhrase(InfType typeInfraction, GrammarTense iTense, bool bAdding)
{
	if (iTense == GrammarTense::PresentOrNoun)
	{
		switch (typeInfraction)
		{
			case Ban:
				return bAdding ? "ban" : "unban";
			case Mute:
				return bAdding ? "mute" : "unmute";
			case Gag:
				return bAdding ? "gag" : "ungag";
			case Silence:
				return bAdding ? "silence" : "unsilence";
			case AdminChatGag:
				return bAdding ? "admin chat gag" : "admin chat ungag";
			case CallAdminBlock:
				return bAdding ? "call admin ban" : "call admin unban";
			case Warn:
				return bAdding ? "warn" : "un-warn";
		}
	}
	else if (iTense == GrammarTense::Past)
	{
		switch (typeInfraction)
		{
			case Ban:
				return bAdding ? "banned" : "unbanned";
			case Mute:
				return bAdding ? "muted" : "unmuted";
			case Gag:
				return bAdding ? "gagged" : "ungagged";
			case Silence:
				return bAdding ? "silenced" : "unsilenced";
			case AdminChatGag:
				return bAdding ? "admin chat gagged" : "admin chat ungagged";
			case CallAdminBlock:
				return bAdding ? "call admin banned" : "call admin unbanned";
			case Warn:
				return bAdding ? "warned" : "un-warned";
		}
	}
	else if (iTense == GrammarTense::Continuous)
	{
		switch (typeInfraction)
		{
			case Ban:
				return bAdding ? "banning" : "unbanning";
			case Mute:
				return bAdding ? "muting" : "unmuting";
			case Gag:
				return bAdding ? "gagging" : "ungagging";
			case Silence:
				return bAdding ? "silencing" : "unsilencing";
			case AdminChatGag:
				return bAdding ? "admin chat gagging" : "admin chat ungagging";
			case CallAdminBlock:
				return bAdding ? "call admin banning" : "call admin unbanning";
			case Warn:
				return bAdding ? "warning" : "un-warning";
		}
	}
	return "";
}

// Converts an InfType to a GFLBans infraction string
const char* LocalToWebInfraction(InfType typeInfraction)
{
	switch (typeInfraction)
	{
		case Mute:
			return "voice_block";
		case Gag:
			return "chat_block";
		case Ban:
			return "ban";
		case AdminChatGag:
			return "admin_chat_block";
		case CallAdminBlock:
			return "call_admin_block";
		case Silence:
			return "silence"; // Since Silence is both voice_block and chat_block, this needs to be fixed outside of the function
	}
	return "";
}

// Returns json player objects for use in GFLBans Queries
json PlayerObj(CHandle<CCSPlayerController> hPlayer, bool bUseIP)
{
	if (!hPlayer)
		return json();

	return PlayerObj(hPlayer.Get(), bUseIP);
}

// Returns json player objects for use in GFLBans Queries
json PlayerObj(CCSPlayerController* pPlayer, bool bUseIP)
{
	if (!pPlayer)
		return json();

	return PlayerObj(pPlayer->GetZEPlayer(), bUseIP);
}

// Returns json player objects for use in GFLBans Queries
json PlayerObj(ZEPlayer* zpPlayer, bool bUseIP)
{
	if (!zpPlayer)
		return json();

	json jRequestBody;
	jRequestBody["gs_service"] = "steam";

	if (zpPlayer->IsFakeClient())
		jRequestBody["gs_id"] = "BOT";
	else
		jRequestBody["gs_id"] = std::to_string(zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64());

	if (bUseIP)
	{
		std::string strIP = zpPlayer->GetIpAddress();
		if (IsValidIP(strIP))
			jRequestBody["ip"] = strIP;
	}

	return jRequestBody;
}

// Returns a URL to query zpPlayer's current GFLBans infractions
std::string PlayerQuery(ZEPlayer* zpPlayer, bool bUseIP)
{
	if (zpPlayer->IsFakeClient())
		return "";

	std::string strURL = g_strGFLBansApiUrl + "infractions/check?gs_service=steam&gs_id=";
	strURL.append(std::to_string(zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64()));
	if (g_bGFLBansAcceptGlobal)
		strURL.append("&include_other_servers=false");

	if (bUseIP)
	{
		std::string strIP = zpPlayer->GetIpAddress();
		if (IsValidIP(strIP))
			strURL.append("&ip=" + strIP);
	}

#ifdef _DEBUG
	Message(("Request URL: " + strURL + "\n").c_str());
#endif
	return strURL;
}

// Does a VERY basic check to make sure IP is kind of valid and not local
bool IsValidIP(std::string strIP)
{
	return strIP.length() > 6 && strIP != "127.0.0.1" && std::find_if(strIP.begin(), strIP.end(), [](unsigned char c) { return !std::isdigit(c) && c != '.'; }) == strIP.end();
}

void EchoMessage(CCSPlayerController* pAdmin, CCSPlayerController* pTarget, const char* pszPunishment, EchoType echo)
{
	const char* pszAdminName = pAdmin ? pAdmin->GetPlayerName() : CONSOLE_NAME;

	switch (echo)
	{
		case EchoType::All:
			ClientPrintAll(HUD_PRINTTALK, GFLBANS_PREFIX ADMIN_PREFIX "%s.", pszAdminName, pszPunishment);
			break;
		case EchoType::Admin:
			ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX ADMIN_PREFIX "%s.", pszAdminName, pszPunishment);
			break;
		case EchoType::Target:
			ClientPrint(pTarget, HUD_PRINTTALK, GFLBANS_PREFIX ADMIN_PREFIX "%s.", pszAdminName, pszPunishment);
			break;
		case EchoType::Console:
			ClientPrint(nullptr, HUD_PRINTTALK, GFLBANS_PREFIX ADMIN_PREFIX "%s.", pszAdminName, pszPunishment);
			break;
	}
}