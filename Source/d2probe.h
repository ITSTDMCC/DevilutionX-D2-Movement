/**
 * @file d2probe.h
 *
 * Probes for the Diablo 2 movement mod. Every function the mod touches, and the engine functions
 * that depend on player movement, count how often they run. When the environment variable
 * D2PROBE_LOG names a file, the probes also write events, per-tick invariant failures, level
 * hashes and a counter dump there for tools/d2harness to filter into pass/fail.
 *
 * Counting is a single increment, and nothing here changes game state, so probes are always compiled in.
 *
 * Part of DevilutionX-D2-Movement. Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
 * Licensed under the Sustainable Use License (LICENSE.md); see NOTICE-D2MOVEMENT.md.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace devilution {

namespace d2probe {

// clang-format off
#define D2PROBE_LIST(X) \
	/* d2mod.cpp: the mod itself */ \
	X(d2_FreeMoveSetTarget) X(d2_FreeMoveTick) X(d2_FreeMoveStop) X(d2_FreeMoveReset) X(d2_FreeMoveInterrupt) \
	X(d2_FreeMoveTargetTile) X(d2_GlideCorrection) X(d2_SetCursorFine) X(d2_SendWalkToCursor) \
	X(d2_OnWalkFine) X(d2_ToggleRun) X(d2_UpdateLocalRunState) X(d2_ShareRunState) \
	X(d2_StraightenPath) X(d2_MoveTo) X(d2_MoveBlocked) X(d2_DirectionVector) \
	X(d2_ChanceToHit) X(d2_PlayerFlinches) X(d2_MonsterFlinches) \
	/* player.cpp: touched */ \
	X(player_IsPlayerRunning) X(player_StartWalkAnimation) X(player_InitLevelChange) X(player_DoWalk) \
	X(player_CheckNewPath) X(player_GetTargetPosition) X(player_getGraphic) X(player_CreatePlayer) \
	X(player_InitPlayer) X(player_ProcessPlayers) X(player_ClrPlrPath) X(player_MakePlrPath) \
	X(player_PlrHitMonst) X(player_PlrHitPlr) X(player_StartPlrHit) \
	/* other touched files */ \
	X(diablo_LeftMouseCmd) X(diablo_GameLogic) X(cursor_CheckCursMove) X(track_RepeatWalk) \
	X(msg_OnWalk) X(msg_OnWalkFine) X(msg_OnSetRun) \
	X(scrollrt_DrawPlayerHelper) X(scrollrt_CalcFirstTilePosition) X(scrollrt_DrawGame) \
	X(automap_DrawAutomapPlr) X(automap_DrawAutomap) \
	X(missiles_MonsterMHit) X(missiles_Plr2PlrMHit) X(missiles_PlayerMHit) \
	X(monster_MonsterAttackPlayer) X(monster_M_StartHit) \
	/* affected: engine code that reads or reacts to player movement */ \
	X(path_FindPath) X(player_PosOkPlayer) X(lighting_ChangeLightXY) X(lighting_ChangeLightOffset) \
	X(lighting_ChangeVisionXY) X(autopickup_AutoPickup) X(sound_PlaySfxLoc) X(sound_PlaySFX) \
	X(trigs_CheckTriggers) X(monster_UpdateEnemy) X(msg_NetSendCmdLoc) X(player_FixPlayerLocation) \
	X(player_StartStand) X(player_StartWalk) X(diablo_LoadGameLevel)
// clang-format on

enum class Id : uint16_t {
#define D2PROBE_ENUM(name) name,
	D2PROBE_LIST(D2PROBE_ENUM)
#undef D2PROBE_ENUM
	    Count
};

extern uint64_t Counters[static_cast<size_t>(Id::Count)];

inline void Hit(Id id)
{
	Counters[static_cast<size_t>(id)]++;
}

/**
 * A probe site for the many functions that only depend on movement state (see blast_radius.md):
 * a constant-initialised static per function, put on the dump list the first time it is hit.
 */
struct Site {
	const char *function;
	const char *file;
	uint64_t hits;
	Site *next;
	bool registered;
};

void RegisterSite(Site &site);

inline void HitSite(Site &site)
{
	site.hits++;
	if (!site.registered)
		RegisterSite(site);
}

/** @brief Current value of one counter. */
uint64_t Count(Id id);

/** @brief Is D2PROBE_LOG set (events and invariant checks are on)? */
bool LogEnabled();

/** @brief Write one event line, prefixed with the current game tick. */
void Event(std::string_view line);

/** @brief Record a failed invariant: counted, and written as a FAIL line when logging. */
void Fail(std::string_view check, std::string_view detail);

uint64_t FailureCount();

/** @brief Per game tick invariant checks for every player on the level (when logging or forced). */
void CheckTick();

/** @brief Run the invariant checks even without a log file (unit tests). */
void ForceChecks(bool on);

/** @brief Hash the freshly loaded level (dungeon, monsters, objects, items) into a LEVEL line. */
void OnLevelLoaded();

/** @brief Record a completed tick of free movement for the speed statistics. */
void RecordMoveTick(bool running, int64_t distance);

/** @brief Timedemo result for the FPS benchmark. */
void RecordBench(int frames, float seconds);

/** @brief Write all counters and statistics (also runs automatically at exit). */
void Dump(std::string_view reason);

/** @brief Zero every counter and statistic (used between harness scenarios). */
void Reset();

} // namespace d2probe

} // namespace devilution

#define D2_PROBE(name) ::devilution::d2probe::Hit(::devilution::d2probe::Id::name)
#define D2_PROBE_FN()                                                                  	do {                                                                               		static ::devilution::d2probe::Site d2probeSite { __func__, __FILE__, 0, nullptr, false }; 		::devilution::d2probe::HitSite(d2probeSite);                                   	} while (false)
