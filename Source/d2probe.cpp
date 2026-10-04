/**
 * @file d2probe.cpp
 *
 * Probe counters, event log and invariant checks for the Diablo 2 movement mod (see d2probe.h).
 *
 * Part of DevilutionX-D2-Movement. Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
 * Licensed under the Sustainable Use License (LICENSE.md); see NOTICE-D2MOVEMENT.md.
 */
#include "d2probe.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "d2mod.h"
#include "engine/path.h"
#include "items.h"
#include "levels/gendung.h"
#include "monster.h"
#include "objects.h"
#include "player.h"

namespace devilution {

namespace d2probe {

uint64_t Counters[static_cast<size_t>(Id::Count)] = {};

namespace {

constexpr const char *ProbeNames[] = {
#define D2PROBE_NAME(name) #name,
	D2PROBE_LIST(D2PROBE_NAME)
#undef D2PROBE_NAME
};

std::FILE *LogFile = nullptr;
bool LogChecked = false;
bool ChecksForced = false;
uint64_t Tick = 0;
uint64_t Failures = 0;
uint64_t FailLinesWritten = 0;

struct MoveStats {
	uint64_t ticks = 0;
	int64_t moved = 0;
	int64_t expected = 0;
};
MoveStats WalkStats;
MoveStats RunStats;

struct LastPosition {
	bool valid = false;
	int32_t x = 0;
	int32_t y = 0;
	Point tile;
	uint8_t level = 0;
	bool active = false;
};
LastPosition LastPositions[MAX_PLRS];
Site *Sites = nullptr;

void Write(const std::string &line)
{
	if (LogFile == nullptr)
		return;
	std::fputs(line.c_str(), LogFile);
	std::fputc('\n', LogFile);
}

void DumpAtExit()
{
	Dump("exit");
	if (LogFile != nullptr) {
		std::fclose(LogFile);
		LogFile = nullptr;
	}
}

uint64_t Fnv(uint64_t hash, const void *data, size_t size)
{
	const auto *bytes = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

template <typename T>
uint64_t FnvValue(uint64_t hash, const T &value)
{
	return Fnv(hash, &value, sizeof(value));
}

} // namespace

void RegisterSite(Site &site)
{
	site.registered = true;
	site.next = Sites;
	Sites = &site;
}

uint64_t Count(Id id)
{
	return Counters[static_cast<size_t>(id)];
}

bool LogEnabled()
{
	if (!LogChecked) {
		LogChecked = true;
		const char *path = std::getenv("D2PROBE_LOG");
		if (path != nullptr && path[0] != '\0') {
			LogFile = std::fopen(path, "w");
			if (LogFile != nullptr) {
				std::atexit(DumpAtExit);
				Write("D2PROBE v1");
			}
		}
	}
	return LogFile != nullptr;
}

void Event(std::string_view line)
{
	if (!LogEnabled())
		return;
	Write(fmt::format("T{} {}", Tick, line));
}

void Fail(std::string_view check, std::string_view detail)
{
	Failures++;
	// Keep the log readable if something goes badly wrong: the count is still exact
	if (LogEnabled() && FailLinesWritten < 200) {
		FailLinesWritten++;
		Write(fmt::format("FAIL T{} {} {}", Tick, check, detail));
	}
}

uint64_t FailureCount()
{
	return Failures;
}

void ForceChecks(bool on)
{
	ChecksForced = on;
}

void CheckTick()
{
	if (!LogEnabled() && !ChecksForced)
		return;
	Tick++;
	const bool d2 = d2::MovementEnabled();
	for (const Player &player : Players) {
		const size_t id = player.getId();
		if (id >= MAX_PLRS)
			continue;
		LastPosition &last = LastPositions[id];
		if (!player.plractive || !player.isOnActiveLevel() || player._pHitPoints <= 0) {
			last.valid = false;
			continue;
		}
		const Point tile = player.position.tile;
		if (!InDungeonBounds(tile)) {
			Fail("player_out_of_bounds", fmt::format("p{} ({},{})", id, tile.x, tile.y));
			continue;
		}
		if (player._pmode == PM_STAND) {
			if (dPlayer[tile.x][tile.y] != static_cast<int8_t>(id + 1))
				Fail("player_grid_mismatch", fmt::format("p{} ({},{}) dPlayer={}", id, tile.x, tile.y, dPlayer[tile.x][tile.y]));
			if (IsTileSolid(tile))
				Fail("player_in_solid_tile", fmt::format("p{} ({},{})", id, tile.x, tile.y));
			const int16_t monster = dMonster[tile.x][tile.y];
			// Stock rule (PosOkPlayer): heroes may step onto the tile of a monster that is already dying
			const bool dying = leveltype != DTYPE_TOWN && monster > 0 && (Monsters[monster - 1].hitPoints >> 6) <= 0;
			if (monster != 0 && !dying)
				Fail("player_monster_overlap", fmt::format("p{} ({},{}) dMonster={}", id, tile.x, tile.y, monster));
		}
		if (!d2) {
			if (player.freeMove.valid || player.freeMove.active || player.isRunning)
				Fail("d1_mode_free_move_state", fmt::format("p{}", id));
			continue;
		}
		const d2::FreeMoveState &move = player.freeMove;
		if (move.valid) {
			const int32_t dx = move.x - tile.x * d2::SubTile;
			const int32_t dy = move.y - tile.y * d2::SubTile;
			// While moving, the hero's exact point must lie in the tile the rest of the game sees. (When stopped it may
			// be stale after a teleport or level change; the next order resyncs it, see EnsurePosition.)
			if (move.active && (std::abs(dx) > d2::SubTile / 2 || std::abs(dy) > d2::SubTile / 2))
				Fail("sub_tile_outside_tile", fmt::format("p{} ({},{}) d=({},{})", id, tile.x, tile.y, dx, dy));
			if (last.valid && last.active && move.active && last.level == player.plrlevel && last.tile.WalkingDistance(tile) <= 1) {
				const int64_t mx = move.x - last.x;
				const int64_t my = move.y - last.y;
				// Never faster than a stock Diablo 1 step: walking in the dungeon, jogging in town
				const int64_t limit = (leveltype == DTYPE_TOWN ? d2::D1TownRunSpeed : d2::D1WalkSpeed) + 1;
				if (std::max(std::abs(mx), std::abs(my)) > limit)
					Fail("speed_above_d1", fmt::format("p{} moved ({},{}) in one tick", id, mx, my));
			}
			last.valid = true;
			last.x = move.x;
			last.y = move.y;
			last.tile = tile;
			last.level = player.plrlevel;
			last.active = move.active;
		} else {
			last.valid = false;
		}
	}
}

void OnLevelLoaded()
{
	if (!LogEnabled())
		return;
	uint64_t hash = 1469598103934665603ULL;
	hash = Fnv(hash, dungeon, sizeof(dungeon));
	hash = Fnv(hash, dPiece, sizeof(dPiece));
	uint64_t monsters = 1469598103934665603ULL;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		monsters = FnvValue(monsters, monster.type().type);
		monsters = FnvValue(monsters, monster.position.tile);
		monsters = FnvValue(monsters, monster.maxHitPoints);
	}
	uint64_t objects = 1469598103934665603ULL;
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		objects = FnvValue(objects, object._otype);
		objects = FnvValue(objects, object.position);
	}
	uint64_t items = 1469598103934665603ULL;
	for (uint8_t i = 0; i < ActiveItemCount; i++) {
		const Item &item = Items[ActiveItems[i]];
		items = FnvValue(items, item.IDidx);
		items = FnvValue(items, item._iSeed);
		items = FnvValue(items, item.position);
	}
	Event(fmt::format("LEVEL {} type={} map={:016x} monsters={}:{:016x} objects={}:{:016x} items={}:{:016x}",
	    currlevel, static_cast<int>(leveltype), hash, ActiveMonsterCount, monsters, ActiveObjectCount, objects, ActiveItemCount, items));
}

void RecordMoveTick(int32_t expected, int64_t moved)
{
	MoveStats &stats = expected > d2::D1WalkSpeed ? RunStats : WalkStats;
	stats.ticks++;
	stats.moved += moved;
	stats.expected += expected;
}

void RecordBench(int frames, float seconds)
{
	Event(fmt::format("BENCH frames={} seconds={:.3f} fps={:.1f}", frames, seconds, seconds > 0 ? frames / seconds : 0.0F));
}

void Dump(std::string_view reason)
{
	if (!LogEnabled())
		return;
	Write(fmt::format("DUMP {} mode={} ticks={}", reason, d2::MovementEnabled() ? "d2" : "d1", Tick));
	for (size_t i = 0; i < static_cast<size_t>(Id::Count); i++)
		Write(fmt::format("COUNT {} {}", ProbeNames[i], Counters[i]));
	for (const Site *site = Sites; site != nullptr; site = site->next) {
		std::string_view file = site->file;
		const size_t slash = file.find_last_of("/\\");
		if (slash != std::string_view::npos)
			file.remove_prefix(slash + 1);
		Write(fmt::format("SITE {}:{} {}", file, site->function, site->hits));
	}
	Write(fmt::format("STAT walk_ticks {} walk_moved {} walk_expected {}", WalkStats.ticks, WalkStats.moved, WalkStats.expected));
	Write(fmt::format("STAT run_ticks {} run_moved {} run_expected {}", RunStats.ticks, RunStats.moved, RunStats.expected));
	Write(fmt::format("STAT failures {}", Failures));
	std::fflush(LogFile);
}

void Reset()
{
	for (uint64_t &counter : Counters)
		counter = 0;
	for (Site *site = Sites; site != nullptr; site = site->next)
		site->hits = 0;
	Tick = 0;
	Failures = 0;
	FailLinesWritten = 0;
	WalkStats = {};
	RunStats = {};
	for (LastPosition &last : LastPositions)
		last = {};
}

} // namespace d2probe

} // namespace devilution
