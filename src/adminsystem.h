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
#include "platform.h"
#include "utlvector.h"
#include "playermanager.h"
#include <ctime>
#include "ctimer.h"
#include "entity/ccsplayercontroller.h"
#include <string>
#include <memory>
#include <vector>
#include <utility>
#include "httpmanager.h"
#include "vendor/nlohmann/json.hpp"
using json = nlohmann::json;

#define ADMFLAG_NONE		(0)
#define ADMFLAG_RESERVATION (1 << 0)  // a
#define ADMFLAG_GENERIC		(1 << 1)  // b
#define ADMFLAG_KICK		(1 << 2)  // c
#define ADMFLAG_BAN			(1 << 3)  // d
#define ADMFLAG_UNBAN		(1 << 4)  // e
#define ADMFLAG_SLAY		(1 << 5)  // f
#define ADMFLAG_CHANGEMAP	(1 << 6)  // g
#define ADMFLAG_CONVARS		(1 << 7)  // h
#define ADMFLAG_CONFIG		(1 << 8)  // i
#define ADMFLAG_CHAT		(1 << 9)  // j
#define ADMFLAG_VOTE		(1 << 10) // k
#define ADMFLAG_PASSWORD	(1 << 11) // l
#define ADMFLAG_RCON		(1 << 12) // m
#define ADMFLAG_CHEATS		(1 << 13) // n
#define ADMFLAG_CUSTOM1		(1 << 14) // o
#define ADMFLAG_CUSTOM2		(1 << 15) // p
#define ADMFLAG_CUSTOM3		(1 << 16) // q
#define ADMFLAG_CUSTOM4		(1 << 17) // r
#define ADMFLAG_CUSTOM5		(1 << 18) // s
#define ADMFLAG_CUSTOM6		(1 << 19) // t
#define ADMFLAG_CUSTOM7		(1 << 20) // u
#define ADMFLAG_CUSTOM8		(1 << 21) // v
#define ADMFLAG_CUSTOM9		(1 << 22) // w
#define ADMFLAG_CUSTOM10	(1 << 23) // x
#define ADMFLAG_CUSTOM11	(1 << 24) // y
#define ADMFLAG_ROOT		(1 << 25) // z

#define ADMIN_PREFIX "Admin %s has "
#define GFLBANS_PREFIX " \x02[GFLBans]\1 "

void PrintSingleAdminAction(const char* pszAdminName, const char* pszTargetName, const char* pszAction, const char* pszAction2, const char* prefix);
void PrintMultiAdminAction(ETargetType nType, const char* pszAdminName, const char* pszAction, const char* pszAction2, const char* prefix);
class GFLBans_PlayerObjNoIp
{
public:
	std::string m_strGSID; // Unique Steam64 ID of player

	GFLBans_PlayerObjNoIp(std::string strGSID) : m_strGSID(strGSID) {}
	GFLBans_PlayerObjNoIp(ZEPlayer* player);

	virtual json CreateInfractionJSON() const;
	// Returns a GFLBans GET Query for the player's Steam64 ID to check for active punishments
	virtual std::string GB_Query() const;
};

class GFLBans_PlayerObjIPOptional : public GFLBans_PlayerObjNoIp
{
public:
	std::string m_strIP; // WAN address of player.

	GFLBans_PlayerObjIPOptional(std::string strGSID, std::string strIP = "") :
		GFLBans_PlayerObjNoIp(strGSID), m_strIP(strIP)
	{}
	GFLBans_PlayerObjIPOptional(ZEPlayer* player);

	virtual json CreateInfractionJSON() const;
	bool HasIP() const;
	// Returns a GFLBans GET Query for the player's IP to check for active punishments.
	virtual std::string GB_Query() const override;
};
typedef GFLBans_PlayerObjIPOptional GFLBans_PlayerObjSimple;

class GFLBans_InfractionBase
{
public:
	enum GFLInfractionType
	{
		Mute,
		Gag,
		Ban,
		AdminChatGag,
		CallAdminBlock,
		Silence
	};

	enum GFLInfractionScope
	{
		Server,
		Global
	};

	GFLBans_InfractionBase(std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson, GFLInfractionType gitType,
						   std::string strReason, std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin = nullptr);

	virtual json CreateInfractionJSON() const = 0;
	auto GetInfractionType() const noexcept -> GFLInfractionType;
	std::string GetReason() const noexcept { return m_strReason; }
	virtual ~GFLBans_InfractionBase() {}

protected:
	std::shared_ptr<GFLBans_PlayerObjSimple> m_plyBadPerson;
	std::shared_ptr<GFLBans_PlayerObjNoIp> m_wAdmin;
	GFLInfractionType m_gitType;
	std::string m_strReason;
};

class GFLBans_Infraction : public GFLBans_InfractionBase
{
public:
	GFLBans_Infraction(std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson, GFLInfractionType gitType,
					   std::string strReason, std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin = nullptr,
					   int iDuration = -1, bool bOnlineOnly = false);

	bool IsSession() const noexcept;

	// Creates a JSON object to pass in a POST request to GFLBans
	virtual json CreateInfractionJSON() const override;
private:
	std::string m_strID;
	int m_iFlags;
	uint m_wCreated; // UNIX timestamp
	uint m_wExpires; // UNIX timestamp
	GFLInfractionScope m_gisScope;
	bool m_bOnlineOnly;
	std::string m_strPolicyID; // For tiering policy
};

class GFLBans_RemoveInfractionsOfPlayer : public GFLBans_InfractionBase
{
public:
	GFLBans_RemoveInfractionsOfPlayer(std::shared_ptr<GFLBans_PlayerObjSimple> plyBadPerson,
									  GFLInfractionType gitType, std::string strReason,
									  std::shared_ptr<GFLBans_PlayerObjNoIp> plyAdmin = nullptr) :
		GFLBans_InfractionBase(plyBadPerson, gitType, strReason, plyAdmin)
	{}

	// Creates a JSON object to pass in a POST request to GFLBans
	virtual json CreateInfractionJSON() const override;
};

class GFLBans_Report
{
public:
	GFLBans_Report(std::shared_ptr<GFLBans_PlayerObjNoIp> plyCaller, std::string strCallerName,
				   std::string strMessage, std::shared_ptr<GFLBans_PlayerObjNoIp> plyBadPerson = nullptr,
				   std::string strBadPersonName = "");

	json CreateReportJSON() const;
	bool IsReport() const noexcept;
	void GFLBans_CallAdmin(CCSPlayerController* pCaller);
	virtual ~GFLBans_Report() {}

protected:
	std::shared_ptr<GFLBans_PlayerObjNoIp> m_plyBadPerson;
	std::string m_strBadPersonName;
	std::shared_ptr<GFLBans_PlayerObjNoIp> m_plyCaller;
	std::string m_strCallerName;
	std::string m_strMessage;
};

class CInfractionBase
{
public:
	CInfractionBase(time_t duration, uint64 steamId, bool bEndTime = false, bool bSession = false) :
		m_iSteamID(steamId), m_bSession(bSession)
	{
		// The duration is in minutes here
		if (!bEndTime)
			m_iTimestamp = duration != 0 ? std::time(nullptr) + (duration * 60) : 0;
		else
			m_iTimestamp = duration;
	}
	enum EInfractionType
	{
		Ban,
		Mute,
		Gag
	};

	virtual EInfractionType GetType() = 0;
	virtual void ApplyInfraction(ZEPlayer*) = 0;
	virtual void UndoInfraction(ZEPlayer *) = 0;
	time_t GetTimestamp() { return m_iTimestamp; }
	uint64 GetSteamId64() { return m_iSteamID; }
	virtual bool IsSession() const noexcept { return m_bSession; }

private:
	time_t m_iTimestamp;
	uint64 m_iSteamID;
	bool m_bSession;
};

class CBanInfraction : public CInfractionBase
{
public:
	using CInfractionBase::CInfractionBase;

	EInfractionType GetType() override { return Ban; }
	void ApplyInfraction(ZEPlayer*) override;

	// This isn't needed as we'll just not kick the player when checking infractions upon joining
	void UndoInfraction(ZEPlayer *) override {}
	bool IsSession() const noexcept override { return false; }
};

class CMuteInfraction :public CInfractionBase
{
public:
	using CInfractionBase::CInfractionBase;

	EInfractionType GetType() override { return Mute; }
	void ApplyInfraction(ZEPlayer*) override;
	void UndoInfraction(ZEPlayer *) override;
};

class CGagInfraction : public CInfractionBase
{
public:
	using CInfractionBase::CInfractionBase;

	EInfractionType GetType() override { return Gag; }
	void ApplyInfraction(ZEPlayer *) override;
	void UndoInfraction(ZEPlayer *) override;
};

class CAdmin
{
public:
	CAdmin(const char* pszName, uint64 iSteamID, uint64 iFlags) :
		m_pszName(pszName), m_iSteamID(iSteamID), m_iFlags(iFlags)
	{}

	const char* GetName() { return m_pszName; };
	uint64 GetSteamID() { return m_iSteamID; };
	uint64 GetFlags() { return m_iFlags; };

private:
	const char* m_pszName;
	uint64 m_iSteamID;
	uint64 m_iFlags;
};

class CAdminSystem
{
public:
	// When was the last time a heartbeat occured
	std::time_t m_wLastHeartbeat;
	std::map<uint64, std::pair<std::shared_ptr<GFLBans_Report>, CTimerBase*>> mapPendingReports;

	CAdminSystem();

	// This forcibly resyncs all blocks with the web. It does NOT tell GFLBans to remove blocks
	void RemoveAllPunishments();

	// If given a fDelay in seconds, will remove all timed session punishments after that fDelay
	// If given no parameter (map change), will remove all session punishments with 0 duration
	void RemoveSessionPunishments(float fDelay = 0);
	bool LoadAdmins();
	void AddInfraction(CInfractionBase*);
	bool ApplyInfractions(ZEPlayer* player);
	bool FindAndRemoveInfraction(ZEPlayer* player, CInfractionBase::EInfractionType type, bool bRemoveSession = true);
	CAdmin* FindAdmin(uint64 iSteamID);

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
								  CCSPlayerController* pBadPerson, CCSPlayerController* pAdmin);

	// Send a POST request to GFLBans telling it to remove all blocks of a given type for a player
	void GFLBans_RemoveInfraction(std::shared_ptr<GFLBans_RemoveInfractionsOfPlayer> infPunishment,
								  CCSPlayerController* pBadPerson, CCSPlayerController* pAdmin);

	void RemoveInfractionType(ZEPlayer* player, CInfractionBase::EInfractionType itypeToRemove,
							  bool  bRemoveGagAndMute);

	// If bApplyBlock, will attempt to apply block on the server if json contains one
	// Return Values are based on if jAllBlockInfo contains a block, not if it was applied
	bool CheckJSONForBlock(ZEPlayer* player, json jAllBlockInfo,
						   GFLBans_InfractionBase::GFLInfractionType blockType,
						   bool bApplyBlock = true, bool bRemoveSession = true);
	void AddDisconnectedPlayer(const char* pszName, uint64 xuid, const char* pszIP);
	void ShowDisconnectedPlayers(CCSPlayerController* const pAdmin);
	// Checks whether the punishment length is allowed to be an offline punishment based upon
	// gflbans.cfg's PUNISH_OFFLINE_DURATION_MIN value. Perma punishments MUST always be offline.
	bool CanPunishmentBeOffline(int iDuration) const noexcept;
	void DumpInfractions();
	uint64 ParseFlags(const char* pszFlags);

private:
	CUtlVector<CAdmin> m_vecAdmins;
	CUtlVector<CInfractionBase*> m_vecInfractions;

	// Implemented as a circular buffer. First in, first out, with random access
	std::tuple<std::string, uint64, std::string> m_rgDCPly[20];
	int m_iDCPlyIndex;
};

extern CAdminSystem *g_pAdminSystem;

void PrecacheAdminBeaconParticle(IEntityResourceManifest * pResourceManifest);
// Prints out a formatted list of punishments to the player's console.
// Does not include session punishments, as GFLBans will not return those.
void ConsoleListPunishments(CCSPlayerController* const player, json punishments);

// Given a formatted time entered by an admin, return the minutes
int ParseTimeInput(std::string strTime);

// Given a time in seconds, returns a formatted string of the largest (floored) unit of time this exceeds, up to months.
// Example: FormatTime(70) == "1 minute(s)"
std::string FormatTime(std::time_t wTime, bool bInSeconds = true);

// Gets reason from a user command such as mute, gag, ban, etc.
std::string GetReason(const CCommand& args, int iArgsBefore);