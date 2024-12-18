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

#pragma once

#include "ctimer.h"
#include "entity/ccsplayercontroller.h"
#include "httpmanager.h"
#include "vendor/nlohmann/json.hpp"

using json = nlohmann::json;

#define GFLBANS_PREFIX " \x07[GFLBans]\1 "

enum InfType
{
	Ban,
	Mute,
	Gag,
	Silence,
	AdminChatGag,
	CallAdminBlock,
	Warn
};

enum EchoType
{
	None,
	All,
	Admin,
	Target,
	Console
};

enum InfractionFlags
{
	SYSTEM = 1 << 0,
	GLOBAL = 1 << 1,
	COMMUNITY = 1 << 2,
	PERMANENT = 1 << 3,
	VPN = 1 << 4,
	WEB = 1 << 5,
	REMOVED = 1 << 6,
	VOICE_BLOCK = 1 << 7,
	CHAT_BLOCK = 1 << 8,
	BAN = 1 << 9,
	ADMIN_CHAT_BLOCK = 1 << 10,
	CALL_ADMIN_BAN = 1 << 11,
	SESSION = 1 << 12,
	DEC_ONLINE_ONLY = 1 << 13,
	AUTO_TIER = 1 << 16,
	NOT_WARNING = (VOICE_BLOCK | CHAT_BLOCK | BAN | ADMIN_CHAT_BLOCK | CALL_ADMIN_BAN)
};

void EchoMessage(CCSPlayerController* pAdmin, CCSPlayerController* pTarget, const char* pszPunishment, EchoType echo);

// Creates a new infraction of type infType on the server and adds it to GFLBans. pBadPerson must be a valid client
void CreateInfraction(InfType infType, EchoType echo, CCSPlayerController* pAdmin,
					  CCSPlayerController* pBadPerson, std::string strReason, int iDuration,
					  bool bOnlineOnly);

// Removes all infractions of type infType both on the server and on GFLBans. pGoodPerson must be a valid client
void RemoveInfraction(InfType infType, EchoType echo, CCSPlayerController* pAdmin,
					  CCSPlayerController* pGoodPerson, std::string strReason);

class GFLBans_InfractionBase
{
public:
	enum GFLInfractionScope
	{
		Server,
		Global
	};

	GFLBans_InfractionBase(InfType infType, CHandle<CCSPlayerController> hTarget, std::string strReason,
						   CHandle<CCSPlayerController> hAdmin = nullptr);

	virtual json CreateInfractionJSON() const = 0;
	InfType GetInfractionType() const noexcept { return m_infType; }
	std::string GetReason() const noexcept { return m_strReason; }
	virtual ~GFLBans_InfractionBase() {}

protected:
	InfType m_infType;
	CHandle<CCSPlayerController> m_hTarget;
	CHandle<CCSPlayerController> m_hAdmin;
	std::string m_strReason;
};

class GFLBans_Infraction : public GFLBans_InfractionBase
{
public:
	GFLBans_Infraction(InfType infType, CHandle<CCSPlayerController> hTarget, std::string strReason,
					   CHandle<CCSPlayerController> hAdmin = nullptr, int iDuration = -1, bool bOnlineOnly = false);

	bool IsSession() const noexcept;

	// Creates a JSON object to pass in a POST request to GFLBans
	virtual json CreateInfractionJSON() const override;

private:
	std::string m_strID;
	uint m_wCreated; // UNIX timestamp
	uint m_wExpires; // UNIX timestamp
	GFLInfractionScope m_gisScope;
	bool m_bOnlineOnly;
};

class GFLBans_InfractionRemoval : public GFLBans_InfractionBase
{
public:
	GFLBans_InfractionRemoval(InfType infType, CHandle<CCSPlayerController> hTarget, std::string strReason,
							  CHandle<CCSPlayerController> hAdmin = nullptr) :
		GFLBans_InfractionBase(infType, hTarget, strReason, hAdmin)
	{}

	// Creates a JSON object to pass in a POST request to GFLBans
	virtual json CreateInfractionJSON() const override;
};

class GFLBans_Report
{
public:
	GFLBans_Report(CCSPlayerController* hCaller, std::string strMessage, CCSPlayerController* hBadPerson = nullptr);

	json CreateReportJSON() const;
	bool IsReport() const noexcept;
	void GFLBans_CallAdmin(CCSPlayerController* pCaller);
	virtual ~GFLBans_Report() {}

protected:
	json m_jCaller;
	std::string m_strCallerName;
	std::string m_strMessage;
	json m_jBadPerson;
	std::string m_strBadPersonName;
};

class GFLBansSystem
{
public:
	// When was the last time a heartbeat occured
	std::time_t m_wLastHeartbeat;
	std::map<uint64, std::pair<std::shared_ptr<GFLBans_Report>, CTimerBase*>> mapPendingReports;

	// Make sure other functions know these has been no heartbeat yet (they check if it was at most 120 seconds ago)
	GFLBansSystem() { m_wLastHeartbeat = std::time(nullptr) - 121; }

	// Updates m_wLastHeartbeat to the last time the heartbeat successfully got a response from GFLBans
	// Allows GFLBans to do the following:
	// - Know if the server is alive
	// - Update infractions that only decrement while the player is online
	// - Display information about the server in the Web UI
	// returns true if ATTEMPTS to heartbeat, false otherwise. This is not based on if GFLBans responds
	bool GFLBans_Heartbeat();

	// Update g_pAdminSystem with infractions from the web. We are assuming the response json only has 1
	// each type of punishment and not checking for multiples. If there are multiple active, we can just
	// apply a new one when the applied one expires (this is also how we will be able to change currently
	// active punishments that were altered through the web page)
	void GFLBans_CheckPlayerInfractions(ZEPlayer* player);

	// Send a POST request to GFLBans telling it to add a block for a player
	void GFLBans_CreateInfraction(std::shared_ptr<GFLBans_Infraction> infPunishment,
								  CCSPlayerController* pBadPerson, CCSPlayerController* pAdmin,
								  EchoType echo = EchoType::All);

	// Send a POST request to GFLBans telling it to remove all blocks of a given type for a player
	void GFLBans_RemoveInfraction(std::shared_ptr<GFLBans_InfractionRemoval> infPunishment,
								  CCSPlayerController* pGoodPerson, CCSPlayerController* pAdmin,
								  EchoType echo = EchoType::All);

	// If bApplyBlock, will attempt to apply block on the server if json contains one
	// Return Values are based on if jAllBlockInfo contains a block, not if it was applied
	bool CheckJSONForBlock(ZEPlayer* player, json jAllBlockInfo, InfType blockType,
						   bool bApplyBlock = true, bool bRemoveSession = true);

	// Returns true if a chat message should be filtered and false if not
	// If gflbans_filtered_gag_duration is non-negative, pChatter will be gagged for that duration if true return value
	bool FilterMessage(CCSPlayerController* pChatter, const CCommand& args);

	// Prints pBadPerson's longest punishments of each type to pAdmin.
	void CheckPunishmentHistory(CCSPlayerController* pAdmin, CCSPlayerController* pBadPerson,
								std::string strReason);

	// Gets pBadPerson's longest punishment duration of iType and then issues a new punishment with double the duration
	// returns false if it does not send a web request, true if a web request is sent (regardless of whether it returns an error or not)
	bool StackPunishment(CCSPlayerController* pAdmin, CCSPlayerController* pBadPerson,
						 std::string strReason, InfType iType, EchoType echo, bool bWarnFirst);

private:
	void GetPunishmentHistory(std::shared_ptr<std::vector<int>> vecInfractions, int iCounted,
							  CHandle<CCSPlayerController> hAdmin, CHandle<CCSPlayerController> hBadPerson,
							  std::string strReason);

	void DisplayPunishmentHistory(std::shared_ptr<std::vector<int>> vecInfractions,
								  CHandle<CCSPlayerController> hAdmin, CHandle<CCSPlayerController> hBadPerson,
								  std::string strReason);

	bool GetPunishmentStacks(std::shared_ptr<std::vector<int>> vecInfractions, int iCounted,
							 CHandle<CCSPlayerController> hAdmin, CHandle<CCSPlayerController> hBadPerson,
							 std::string strReason, InfType iType, EchoType echo, bool bWarnFirst);

	void ApplyStackedPunishment(std::shared_ptr<std::vector<int>> vecInfractions,
								CHandle<CCSPlayerController> hAdmin, CHandle<CCSPlayerController> hBadPerson,
								std::string strReason, InfType iType, EchoType echo, bool bWarnFirst);
};

extern GFLBansSystem* g_pGFLBansSystem;