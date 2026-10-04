/**
 * @file d2mod.cpp
 *
 * Diablo 2 style movement and combat rules layered on top of the Diablo 1 engine.
 *
 * Part of DevilutionX-D2-Movement. Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
 * Licensed under the Sustainable Use License (LICENSE.md); see NOTICE-D2MOVEMENT.md.
 */
#include "d2mod.h"

#include <algorithm>
#include <cstdlib>
#include <climits>
#include <cstdint>

#include <SDL.h>
#include <fmt/format.h>

#include "engine.h"
#include "cursor.h"
#include "d2probe.h"
#include "effects.h"
#include "engine/animationinfo.h"
#include "levels/gendung.h"
#include "lighting.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "options.h"
#include "player.h"
#include "qol/autopickup.h"
#include "utils/stdcompat/algorithm.hpp"

namespace devilution {

namespace d2 {

namespace {

bool RunToggled = RunByDefault;
/** Last run state sent over the network: -1 means nothing has been sent yet. */
int LastSentRunState = -1;

/** Indexed by 3 * dy + 4 + dx, same layout as the path finder's direction table. */
constexpr int8_t StepCodeByDisplacement[9] = { WALK_N, WALK_NE, WALK_E, WALK_NW, WALK_NONE, WALK_SE, WALK_W, WALK_SW, WALK_S };

int8_t StepCode(Point from, Point to)
{
	D2_PROBE_FN();
	return StepCodeByDisplacement[3 * (to.y - from.y) + 4 + (to.x - from.x)];
}

/** @brief Integer division rounding half away from zero, deterministic on every client. */
int RoundedDiv(int numerator, int denominator)
{
	D2_PROBE_FN();
	if (numerator >= 0)
		return (2 * numerator + denominator) / (2 * denominator);
	return -((-2 * numerator + denominator) / (2 * denominator));
}

/** @brief Point @p i of @p n along the straight line from @p a to @p b. */
Point PointOnLine(Point a, Point b, int i, int n)
{
	D2_PROBE_FN();
	return { a.x + RoundedDiv(i * (b.x - a.x), n), a.y + RoundedDiv(i * (b.y - a.y), n) };
}

bool IsLineWalkable(tl::function_ref<bool(Point)> posOk, Point a, Point b)
{
	D2_PROBE_FN();
	const int n = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
	Point previous = a;
	for (int i = 1; i <= n; i++) {
		Point next = PointOnLine(a, b, i, n);
		if (!posOk(next) || !path_solid_pieces(previous, next))
			return false;
		previous = next;
	}
	return true;
}

/**
 * Diablo 2's tangent table (D2Common, 128 entries, also in D2MOO's Step.cpp): entry t is the unit vector
 * at angle atan(t / 127) scaled to 4096 and rounded down, plus that angle in 64ths of a turn rounded down.
 * Built with integer square roots so every client gets the same values.
 */
struct TangentTable {
	D2Heading entries[128];

	TangentTable()
	    : entries {}
	{
		// First tangent of each 1/64 turn: the smallest t with atan(t / 127) >= k * 5.625 degrees
		constexpr int AngleStarts[7] = { 13, 26, 39, 53, 68, 85, 105 };
		for (int t = 0; t < 128; t++) {
			const int64_t radiusSquared = 127 * 127 + t * t;
			entries[t].x = static_cast<int32_t>(IntSqrtFloor(4096LL * 4096 * t * t / radiusSquared));
			entries[t].y = static_cast<int32_t>(IntSqrtFloor(4096LL * 4096 * 127 * 127 / radiusSquared));
			int angle = 0;
			while (angle < 7 && t >= AngleStarts[angle])
				angle++;
			entries[t].dir64 = angle;
		}
	}

	static int64_t IntSqrtFloor(int64_t value)
	{
		int64_t root = 0;
		int64_t bit = int64_t { 1 } << 62;
		while (bit > value)
			bit >>= 2;
		while (bit != 0) {
			if (value >= root + bit) {
				value -= root + bit;
				root = (root >> 1) + bit;
			} else {
				root >>= 1;
			}
			bit >>= 2;
		}
		return root;
	}
};

const TangentTable &GetTangentTable()
{
	D2_PROBE_FN();
	static const TangentTable Table;
	return Table;
}

} // namespace

bool MovementEnabled()
{
	D2_PROBE_FN();
	return *sgOptions.Gameplay.d2Movement;
}

bool CombatEnabled()
{
	D2_PROBE_FN();
	return *sgOptions.Gameplay.d2Combat;
}

D2Heading D2TangentTableEntry(int tangent)
{
	D2_PROBE_FN();
	return GetTangentTable().entries[clamp(tangent, 0, 127)];
}

D2Heading D2DirectionVector(int64_t dx, int64_t dy)
{
	D2_PROBE(d2_DirectionVector);
	// Same steps as D2Common: fold the delta into the first octant, look up, then unfold
	const bool towardsX = dx >= 0;
	const bool towardsY = dy >= 0;
	int64_t minor = std::abs(dx);
	int64_t major = std::abs(dy);
	const bool xIsMajor = minor > major;
	if (xIsMajor)
		std::swap(minor, major);
	const int tangent = major != 0 ? static_cast<int>(127 * minor / major) : 0;
	const D2Heading &entry = GetTangentTable().entries[tangent];

	D2Heading heading;
	int angle = entry.dir64;
	if (!xIsMajor) {
		heading.x = entry.x;
		heading.y = entry.y;
	} else {
		heading.x = entry.y;
		heading.y = entry.x;
		angle = (-1 - angle) & 0xF;
	}
	if (!towardsY) {
		heading.y = -heading.y;
		angle = (-1 - angle) & 0x1F;
	}
	if (towardsX)
		angle = (-1 - angle) & 0x3F;
	else
		heading.x = -heading.x;
	heading.dir64 = (angle + 8) & 0x3F;
	return heading;
}

Direction FacingForMotion(int64_t dx, int64_t dy, Direction current)
{
	D2_PROBE_FN();
	if (dx == 0 && dy == 0)
		return current;
	// One tile is 64 pixels across and 32 down on screen
	const int64_t sx = (dx - dy) * 32;
	const int64_t sy = (dx + dy) * 16;
	struct Candidate {
		Direction dir;
		int64_t ux;
		int64_t uy;
	};
	// Unit vectors (x1000) of each walk sprite as drawn on screen
	constexpr Candidate Candidates[8] = {
		{ Direction::South, 0, 1000 },
		{ Direction::SouthWest, -894, 447 },
		{ Direction::West, -1000, 0 },
		{ Direction::NorthWest, -894, -447 },
		{ Direction::North, 0, -1000 },
		{ Direction::NorthEast, 894, -447 },
		{ Direction::East, 1000, 0 },
		{ Direction::SouthEast, 894, 447 },
	};
	Direction best = current;
	int64_t bestScore = INT64_MIN;
	int64_t currentScore = INT64_MIN;
	for (const Candidate &candidate : Candidates) {
		const int64_t score = candidate.ux * sx + candidate.uy * sy;
		if (candidate.dir == current)
			currentScore = score;
		if (score > bestScore) {
			bestScore = score;
			best = candidate.dir;
		}
	}
	// Hysteresis: stay with the current sprite while it is within about 4 degrees of the best one
	if (currentScore > 0 && currentScore * 1000 >= bestScore * 998)
		return current;
	return best;
}

Direction FacingFromDir64(int dir64)
{
	D2_PROBE_FN();
	// 0 is +x+y (Diablo 1 South) and both games count towards +y (South West), 8 steps per sprite direction
	return static_cast<Direction>(((dir64 + 4) >> 3) & 7);
}

Displacement WalkStepDisplacement(int8_t step)
{
	D2_PROBE_FN();
	switch (step) {
	case WALK_N: return { -1, -1 };
	case WALK_NE: return { 0, -1 };
	case WALK_E: return { 1, -1 };
	case WALK_SE: return { 1, 0 };
	case WALK_S: return { 1, 1 };
	case WALK_SW: return { 0, 1 };
	case WALK_W: return { -1, 1 };
	case WALK_NW: return { -1, 0 };
	default: return { 0, 0 };
	}
}

int StraightenPath(tl::function_ref<bool(Point)> posOk, Point start, int8_t path[MaxPathLength], int length, uint8_t segmentLengths[MaxPathLength])
{
	D2_PROBE(d2_StraightenPath);
	if (segmentLengths != nullptr) {
		for (size_t k = 0; k < MaxPathLength; k++)
			segmentLengths[k] = 0;
	}
	if (length < 2) {
		if (segmentLengths != nullptr) {
			for (int k = 0; k < length; k++)
				segmentLengths[k] = 1;
		}
		return length;
	}

	// Tiles the original path visits, including the start
	Point waypoints[MaxPathLength + 1];
	waypoints[0] = start;
	for (int i = 0; i < length; i++)
		waypoints[i + 1] = waypoints[i] + WalkStepDisplacement(path[i]);

	int8_t straightened[MaxPathLength];
	uint8_t lengths[MaxPathLength] = {};
	int newLength = 0;
	int i = 0;
	while (i < length) {
		const int segmentStart = newLength;
		// Find the furthest waypoint we can reach in a straight line; the next waypoint always works
		int j = length;
		while (j > i + 1 && !IsLineWalkable(posOk, waypoints[i], waypoints[j]))
			j--;

		if (j == i + 1) {
			straightened[newLength++] = path[i];
		} else {
			const int n = std::max(std::abs(waypoints[j].x - waypoints[i].x), std::abs(waypoints[j].y - waypoints[i].y));
			Point previous = waypoints[i];
			for (int k = 1; k <= n; k++) {
				Point next = PointOnLine(waypoints[i], waypoints[j], k, n);
				straightened[newLength++] = StepCode(previous, next);
				previous = next;
			}
		}
		lengths[segmentStart] = static_cast<uint8_t>(newLength - segmentStart);
		i = j;
	}

	for (int k = 0; k < newLength; k++) {
		path[k] = straightened[k];
		if (segmentLengths != nullptr)
			segmentLengths[k] = lengths[k];
	}
	return newLength;
}

namespace {

/** Tile containing a sub-tile coordinate (tile centres are at multiples of SubTile). */
int TileOf(int32_t v)
{
	D2_PROBE_FN();
	return static_cast<int>((v + SubTile / 2) >> 8);
}

Point TileOf(int32_t x, int32_t y)
{
	D2_PROBE_FN();
	return { TileOf(x), TileOf(y) };
}

int64_t IntSqrt(int64_t value)
{
	D2_PROBE_FN();
	if (value <= 0)
		return 0;
	int64_t x = value;
	int64_t y = (x + 1) / 2;
	while (y < x) {
		x = y;
		y = (x + value / x) / 2;
	}
	return x;
}

bool IsOccupied(const Player &player, Point tile);

/** Can the hero step from tile @p from into the neighbouring tile @p to? */
bool CanCross(const Player &player, Point from, Point to)
{
	D2_PROBE_FN();
	if (from == to)
		return true;
	if (std::abs(to.x - from.x) > 1 || std::abs(to.y - from.y) > 1)
		return false;
	if (!PosOkPlayer(player, to))
		return false;
	return path_solid_pieces(from, to);
}

/** Is the straight line between two sub-tile points free of obstacles? */
bool IsSubTileLineClear(const Player &player, int32_t fromX, int32_t fromY, int32_t toX, int32_t toY)
{
	D2_PROBE_FN();
	// Walk every tile the segment touches, exactly (no sampling, so no corner is ever missed). Tile k covers
	// sub-tile units 256k-128 .. 256k+127.
	const int64_t dx = toX - fromX;
	const int64_t dy = toY - fromY;
	const int stepX = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
	const int stepY = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
	const int64_t adx = std::abs(dx);
	const int64_t ady = std::abs(dy);
	Point tile = TileOf(fromX, fromY);
	const Point end = TileOf(toX, toY);
	// Distance from the start to the first unit of the next tile along each axis
	int64_t nextX = stepX > 0 ? (tile.x * SubTile + SubTile / 2) - fromX : fromX - (tile.x * SubTile - SubTile / 2 - 1);
	int64_t nextY = stepY > 0 ? (tile.y * SubTile + SubTile / 2) - fromY : fromY - (tile.y * SubTile - SubTile / 2 - 1);
	for (int guard = 0; tile != end && guard < 2 * MaxPathLength * 2; guard++) {
		// Which boundary does the segment reach first? Compare nextX / adx with nextY / ady without dividing.
		// Within 2 units of a corner counts as through it (a diagonal step): tile edges round towards +x/+y,
		// so a line meant to pass a corner exactly can miss it by a unit either way.
		const int64_t major = std::max(adx, ady);
		const bool nearCorner = stepX != 0 && stepY != 0 && std::abs(nextX * ady - nextY * adx) * major <= 2 * adx * ady;
		const bool hitX = stepX != 0 && nextX <= adx && (stepY == 0 || nearCorner || nextX * ady < nextY * adx);
		const bool hitY = stepY != 0 && nextY <= ady && (stepX == 0 || nearCorner || nextY * adx < nextX * ady);
		if (!hitX && !hitY)
			break;
		Point next = tile;
		if (hitX)
			next.x += stepX;
		if (hitY)
			next.y += stepY;
		// Exactly through a corner is a diagonal step: allowed wherever Diablo 1 allows one (CanCross), and
		// walked as a corner cut so the hero never touches either side
		if (!CanCross(player, tile, next))
			return false;
		tile = next;
		if (hitX)
			nextX += SubTile;
		if (hitY)
			nextY += SubTile;
	}
	return tile == end;
}

/** Put the hero's sub-tile position back on their tile if something else moved them. */
void EnsurePosition(Player &player)
{
	D2_PROBE_FN();
	FreeMoveState &move = player.freeMove;
	if (move.valid && TileOf(move.x, move.y) == player.position.tile)
		return;
	move.valid = true;
	move.x = player.position.tile.x * SubTile;
	move.y = player.position.tile.y * SubTile;
	move.active = false;
}

bool IsRunning(const Player &player)
{
	D2_PROBE_FN();
	// Running is a town thing, as in stock Diablo; in the dungeon everyone walks
	if (leveltype != DTYPE_TOWN)
		return false;
	return player.isRunning || sgGameInitInfo.bRunInTown != 0;
}

/** Could the hero be at sub-tile point (x, y) next (same tile, or a neighbour they may step into)? */
bool CanMoveTo(const Player &player, int32_t x, int32_t y)
{
	D2_PROBE_FN();
	const Point to = TileOf(x, y);
	return to == player.position.tile || CanCross(player, player.position.tile, to);
}

/** Is a live monster or another player standing on the tile (rather than a wall, door or object)? */
bool IsOccupied(const Player &player, Point tile)
{
	D2_PROBE_FN();
	if (!InDungeonBounds(tile))
		return false;
	const int8_t other = dPlayer[tile.x][tile.y];
	if (other != 0 && std::abs(other) - 1 != static_cast<int>(player.getId()))
		return true;
	return dMonster[tile.x][tile.y] != 0;
}

void UpdateLightOffset(const Player &player)
{
	D2_PROBE_FN();
	if (player.lightId == NO_LIGHT)
		return;
	const FreeMoveState &move = player.freeMove;
	const int32_t dx = move.x - player.position.tile.x * SubTile;
	const int32_t dy = move.y - player.position.tile.y * SubTile;
	// Light offsets are in 1/8 tile steps
	ChangeLightOffset(player.lightId, { static_cast<int8_t>(dx / (SubTile / 8)), static_cast<int8_t>(dy / (SubTile / 8)) });
}

/** Move the hero to a sub-tile point no more than a tile away, switching tiles when needed. */
bool MoveTo(Player &player, int32_t x, int32_t y)
{
	D2_PROBE(d2_MoveTo);
	if (player._pmode != PM_STAND)
		d2probe::Fail("moved_outside_stand", fmt::format("p{} mode={}", player.getId(), static_cast<int>(player._pmode)));
	FreeMoveState &move = player.freeMove;
	const Point from = player.position.tile;
	const Point to = TileOf(x, y);
	if (to != from) {
		if (!CanCross(player, from, to))
			return false;

		const size_t playerId = player.getId();
		dPlayer[from.x][from.y] = 0;
		player.position.tile = to;
		player.position.future = to;
		player.position.old = to;
		dPlayer[to.x][to.y] = static_cast<int8_t>(playerId + 1);
		if (&player == MyPlayer)
			ViewPosition = to;
		if (leveltype != DTYPE_TOWN) {
			ChangeLightXY(player.lightId, to);
			ChangeVisionXY(player.getId(), to);
		}
		move.x = x;
		move.y = y;
		AutoPickup(player);
	} else {
		move.x = x;
		move.y = y;
	}
	if (leveltype != DTYPE_TOWN)
		UpdateLightOffset(player);
	return true;
}

void SetFacing(Player &player, Direction facing)
{
	D2_PROBE_FN();
	FreeMoveState &move = player.freeMove;
	if (move.animating && facing == move.facing)
		return;
	move.facing = facing;
	player._pdir = facing;
	if (!move.animating) {
		NewPlrAnim(player, player_graphic::Walk, facing);
		move.animating = true;
		move.animCarry = 0;
		move.lastStepFrame = -1;
		return;
	}
	// Turn without restarting the stride
	if (!HeadlessMode) {
		int8_t numberOfFrames;
		int8_t ticksPerFrame;
		player.getAnimationFramesAndTicksPerFrame(player_graphic::Walk, numberOfFrames, ticksPerFrame);
		player.AnimInfo.changeAnimationData(player.AnimationData[static_cast<size_t>(player_graphic::Walk)].spritesForDirection(facing), numberOfFrames, ticksPerFrame);
	}
}

void PlayFootstep(Player &player, bool running)
{
	D2_PROBE_FN();
	FreeMoveState &move = player.freeMove;
	const int8_t frame = player.AnimInfo.currentFrame;
	if (frame == move.lastStepFrame)
		return;
	move.lastStepFrame = frame;
	if (*sgOptions.Audio.walkingSound && !running && (frame == 0 || frame == 4))
		PlaySfxLoc(PS_WALK1, player.position.tile);
}

void FinishMoving(Player &player)
{
	D2_PROBE_FN();
	FreeMoveState &move = player.freeMove;
	move.active = false;
	if (move.animating) {
		move.animating = false;
		if (player._pmode == PM_STAND)
			StartStand(player, move.facing);
	}
}

int32_t LastSentFineX = INT32_MIN;
int32_t LastSentFineY = INT32_MIN;
int32_t CursorFineX = 0;
int32_t CursorFineY = 0;

} // namespace

void FreeMoveSetTarget(Player &player, Point tile, int fineX, int fineY, bool endspace)
{
	D2_PROBE(d2_FreeMoveSetTarget);
	EnsurePosition(player);
	FreeMoveState &move = player.freeMove;
	const Point start = player.position.tile;
	fineX = clamp(fineX, -SubTile / 2 + 1, SubTile / 2 - 1);
	fineY = clamp(fineY, -SubTile / 2 + 1, SubTile / 2 - 1);
	const int32_t targetX = tile.x * SubTile + fineX;
	const int32_t targetY = tile.y * SubTile + fineY;

	move.waypointCount = 0;
	move.waypointIndex = 0;
	move.hopEnd = 0;
	move.carryX = 0;
	move.carryY = 0;
	// Walking "into" a chest, a wall or anything else that cannot be stood on (a stick pushing against it, a click
	// on a blocked spot): go next to it and stop there, like stock Diablo 1, rather than shuffling on the spot
	if (endspace && tile != start && !PosOkPlayer(player, tile))
		endspace = false;
	move.goalX = targetX;
	move.goalY = targetY;
	move.goalEndspace = endspace;
	move.repaths = 0;

	if (endspace && IsSubTileLineClear(player, move.x, move.y, targetX, targetY)) {
		// Nothing in the way: head straight for the exact point
		move.waypointX[0] = targetX;
		move.waypointY[0] = targetY;
		move.waypointCount = 1;
	} else if (tile != start) {
		int8_t path[MaxPathLength];
		int length = FindPath([&player](Point position) { return PosOkPlayer(player, position); }, start, tile, path);
		if (length > 0 && !endspace)
			length--;
		if (length > 0) {
			int8_t rawPath[MaxPathLength];
			std::copy(path, path + length, rawPath);
			const int rawLength = length;
			uint8_t segments[MaxPathLength];
			length = StraightenPath([&player](Point position) { return PosOkPlayer(player, position); }, start, path, length, segments);
			Point cursor = start;
			for (int i = 0; i < length; i++) {
				cursor += WalkStepDisplacement(path[i]);
				const bool segmentEnds = i + 1 == length || segments[i + 1] != 0;
				if (segmentEnds && move.waypointCount < MaxMoveWaypoints) {
					move.waypointX[move.waypointCount] = cursor.x * SubTile;
					move.waypointY[move.waypointCount] = cursor.y * SubTile;
					move.waypointCount++;
				}
			}
			// The straightening works tile by tile; check each straight leg exactly and, if any would clip
			// something, walk the path finder's own steps instead (every step is a legal Diablo 1 step)
			bool legsClear = true;
			for (int i = 1; i < move.waypointCount && legsClear; i++)
				legsClear = IsSubTileLineClear(player, move.waypointX[i - 1], move.waypointY[i - 1], move.waypointX[i], move.waypointY[i]);
			if (!legsClear) {
				D2_PROBE(d2_UnstraightenedPath);
				move.waypointCount = 0;
				cursor = start;
				for (int i = 0; i < rawLength && move.waypointCount < MaxMoveWaypoints; i++) {
					cursor += WalkStepDisplacement(rawPath[i]);
					move.waypointX[move.waypointCount] = cursor.x * SubTile;
					move.waypointY[move.waypointCount] = cursor.y * SubTile;
					move.waypointCount++;
				}
			}
			if (endspace && move.waypointCount > 0 && cursor == tile) {
				move.waypointX[move.waypointCount - 1] = targetX;
				move.waypointY[move.waypointCount - 1] = targetY;
			}
			// From off-centre the first leg could clip a corner; go via the tile centre in that case
			if (move.waypointCount > 0 && move.waypointCount < MaxMoveWaypoints
			    && !IsSubTileLineClear(player, move.x, move.y, move.waypointX[0], move.waypointY[0])) {
				for (int i = move.waypointCount; i > 0; i--) {
					move.waypointX[i] = move.waypointX[i - 1];
					move.waypointY[i] = move.waypointY[i - 1];
				}
				move.waypointX[0] = start.x * SubTile;
				move.waypointY[0] = start.y * SubTile;
				move.waypointCount++;
			}
		}
	}

	move.active = move.waypointCount > 0;
}

void FreeMoveTick(Player &player)
{
	if (!MovementEnabled())
		return;
	D2_PROBE(d2_FreeMoveTick);
	EnsurePosition(player);
	FreeMoveState &move = player.freeMove;
	if (player._pmode != PM_STAND) {
		move.active = false;
		move.animating = false;
		return;
	}
	if (!move.active) {
		if (move.animating)
			FinishMoving(player);
		return;
	}

	// Diablo 2 path step: each tick the hero moves along the table heading towards the current path point,
	// re-aiming after every step and carrying on to the next point once it is reached. Distance is counted the
	// Diablo 1 way (larger axis), so every direction is exactly as fast as a stock step.
	const bool running = IsRunning(player);
	const int32_t speed = MoveSpeed(player);
	const int32_t startX = move.x;
	const int32_t startY = move.y;
	const uint8_t startWaypoint = move.waypointIndex;
	bool slid = false;
	bool hopped = false;
	int32_t lastStepX = 0;
	int32_t lastStepY = 0;
	int64_t budget = speed - move.debt;
	move.debt = 0;
	while (budget > 0 && move.active) {
		const int32_t wx = move.waypointX[move.waypointIndex];
		const int32_t wy = move.waypointY[move.waypointIndex];
		const int64_t dx = wx - move.x;
		const int64_t dy = wy - move.y;
		const int64_t steps = std::max(std::abs(dx), std::abs(dy));
		const int64_t budgetBefore = budget;

		int32_t nextX;
		int32_t nextY;
		// A corner cut is a 4 unit hop through the exact corner point, which belongs to neither tile cleanly: always
		// make it in one go rather than stopping on the corner
		if (steps <= budget || steps <= 4) {
			nextX = wx;
			nextY = wy;
			if (steps > budget)
				move.debt = static_cast<int32_t>(steps - budget); // paid back next tick, so pacing stays exact
			budget = std::max<int64_t>(budget - steps, 0);
			move.carryX = 0;
			move.carryY = 0;
			if (!move.animating && steps > 0)
				SetFacing(player, FacingForMotion(dx, dy, move.facing));
		} else {
			const D2Heading heading = D2DirectionVector(dx, dy);
			if (!move.animating)
				SetFacing(player, FacingForMotion(dx, dy, move.facing));
			// Scale the heading so its larger axis moves exactly the budget; keep remainders for the next tick
			const int64_t major = std::max(std::abs(heading.x), std::abs(heading.y));
			const int64_t fineX = budget * heading.x * 4096 / major + move.carryX;
			const int64_t fineY = budget * heading.y * 4096 / major + move.carryY;
			nextX = move.x + static_cast<int32_t>(fineX >> 12);
			nextY = move.y + static_cast<int32_t>(fineY >> 12);
			move.carryX = static_cast<int32_t>(fineX & 0xFFF);
			move.carryY = static_cast<int32_t>(fineY & 0xFFF);
			budget = 0;
		}

		const int32_t beforeX = move.x;
		const int32_t beforeY = move.y;
		if (!MoveTo(player, nextX, nextY)) {
			D2_PROBE(d2_MoveBlocked);
			const Point blockedTile = TileOf(nextX, nextY);
			// Grazing a corner while heading diagonally (a chest, a barrel, a monster or another hero beside the way):
			// Diablo 1 lets heroes step diagonally past them, so cut through the corner point into the diagonal tile
			const int sx = wx > move.x ? 1 : (wx < move.x ? -1 : 0);
			const int sy = wy > move.y ? 1 : (wy < move.y ? -1 : 0);
			const Point here = player.position.tile;
			const Point diagonal = here + Displacement { sx, sy };
			if (!hopped && sx != 0 && sy != 0 && move.waypointCount + 2 <= MaxMoveWaypoints
			    && (blockedTile == here + Displacement { sx, 0 } || blockedTile == here + Displacement { 0, sy })
			    && CanCross(player, here, diagonal)) {
				D2_PROBE(d2_CornerHop);
				hopped = true;
				const int32_t cornerX = here.x * SubTile + sx * (SubTile / 2);
				const int32_t cornerY = here.y * SubTile + sy * (SubTile / 2);
				for (int i = move.waypointCount - 1; i >= move.waypointIndex; i--) {
					move.waypointX[i + 2] = move.waypointX[i];
					move.waypointY[i + 2] = move.waypointY[i];
				}
				// Just inside this tile at the corner, then just inside the diagonal tile
				move.waypointX[move.waypointIndex] = cornerX - 2 * sx;
				move.waypointY[move.waypointIndex] = cornerY - 2 * sy;
				move.waypointX[move.waypointIndex + 1] = cornerX + 2 * sx;
				move.waypointY[move.waypointIndex + 1] = cornerY + 2 * sy;
				move.waypointCount += 2;
				move.hopEnd = static_cast<uint8_t>(move.waypointIndex + 2);
				move.carryX = 0;
				move.carryY = 0;
				budget = budgetBefore; // nothing was spent: the blocked step never happened
				move.debt = 0;
				continue;
			}
			if (IsOccupied(player, blockedTile)) {
				// Someone is in the way: take the way round, as stock path finding would from here; stop only if
				// there is none (then Diablo 1 would not move either)
				if (move.repaths < 3) {
					D2_PROBE(d2_Repath);
					const uint8_t repaths = move.repaths + 1;
					const Point goal = TileOf(move.goalX, move.goalY);
					FreeMoveSetTarget(player, goal, move.goalX - goal.x * SubTile, move.goalY - goal.y * SubTile, move.goalEndspace);
					move.repaths = repaths;
					if (move.active) {
						budget = budgetBefore;
						continue;
					}
				}
				FinishMoving(player);
				return;
			}
			// Clipping the corner of a wall, barrel or other object: slide along it on the axis that is free,
			// trying first whichever axis brings the hero closer to where they are going
			const int32_t slideX[2] = { nextX, move.x };
			const int32_t slideY[2] = { move.y, nextY };
			const bool xFirst = std::abs(nextX - move.x) >= std::abs(nextY - move.y);
			bool moved = false;
			for (int k = 0; k < 2 && !moved; k++) {
				const int c = xFirst ? k : 1 - k;
				if ((slideX[c] != move.x || slideY[c] != move.y) && CanMoveTo(player, slideX[c], slideY[c]))
					moved = MoveTo(player, slideX[c], slideY[c]);
			}
			if (moved) {
				D2_PROBE(d2_MoveSlide);
				slid = true;
				lastStepX = move.x - beforeX;
				lastStepY = move.y - beforeY;
				move.carryX = 0;
				move.carryY = 0;
				break; // re-aim next tick
			}
			// Wedged: look for a new way from here, a few times per order at most
			if (move.repaths < 3) {
				D2_PROBE(d2_Repath);
				const uint8_t repaths = move.repaths + 1;
				const Point goal = TileOf(move.goalX, move.goalY);
				FreeMoveSetTarget(player, goal, move.goalX - goal.x * SubTile, move.goalY - goal.y * SubTile, move.goalEndspace);
				move.repaths = repaths;
				if (move.active)
					break;
			}
			FinishMoving(player);
			return;
		}

		if (move.x != beforeX || move.y != beforeY) {
			lastStepX = move.x - beforeX;
			lastStepY = move.y - beforeY;
		}
		if (move.x == wx && move.y == wy) {
			move.waypointIndex++;
			if (move.waypointIndex >= move.waypointCount)
				move.active = false;
		}
	}

	// Face the way the hero is going on screen. Single ticks are too short to judge by (rounding makes them wobble),
	// so aim at the path point the hero is walking to, looking past the 4 unit hop through a corner cut; when
	// sliding along an obstacle, face the way the slide actually went.
	if (slid) {
		SetFacing(player, FacingForMotion(lastStepX, lastStepY, move.facing));
	} else if (move.active) {
		int j = move.waypointIndex;
		while (j + 1 < move.waypointCount && j < move.hopEnd && std::max(std::abs(move.waypointX[j] - move.x), std::abs(move.waypointY[j] - move.y)) < 8)
			j++;
		SetFacing(player, FacingForMotion(move.waypointX[j] - move.x, move.waypointY[j] - move.y, move.facing));
	} else if (lastStepX != 0 || lastStepY != 0) {
		SetFacing(player, FacingForMotion(lastStepX, lastStepY, move.facing));
	}

	if (!move.active) {
		FinishMoving(player);
		return;
	}

	if (move.waypointIndex == startWaypoint && !slid) {
		// A whole tick on one straight leg: the distance covered (larger axis) is exactly the speed
		const int64_t movedX = move.x - startX;
		const int64_t movedY = move.y - startY;
		d2probe::RecordMoveTick(speed, std::max(std::abs(movedX), std::abs(movedY)));
	}

	// The engine shows one walk frame per tick, which is Diablo 1 walking speed; show extra frames when jogging in
	// town to keep the stride in step. Footsteps follow the Diablo 1 rule: frames 0 and 4, walking only.
	PlayFootstep(player, running);
	move.animCarry += speed - D1WalkCycleSpeed;
	while (move.animCarry >= D1WalkCycleSpeed) {
		move.animCarry -= D1WalkCycleSpeed;
		player.AnimInfo.processAnimation();
		PlayFootstep(player, running);
	}
}

int32_t MoveSpeed(const Player &player)
{
	D2_PROBE_FN();
	return IsRunning(player) ? D1TownRunSpeed : D1WalkSpeed;
}

bool FreeMoveActive(const Player &player)
{
	D2_PROBE_FN();
	return player.freeMove.active;
}

void FreeMoveStop(Player &player)
{
	D2_PROBE(d2_FreeMoveStop);
	// The walk animation is replaced by whatever the hero does next, or by standing on the next tick
	player.freeMove.active = false;
}

void FreeMoveInterrupt(Player &player)
{
	D2_PROBE(d2_FreeMoveInterrupt);
	player.freeMove.active = false;
	player.freeMove.animating = false;
}

void FreeMoveReset(Player &player)
{
	D2_PROBE(d2_FreeMoveReset);
	player.freeMove = {};
}

Point FreeMoveTargetTile(const Player &player)
{
	D2_PROBE(d2_FreeMoveTargetTile);
	const FreeMoveState &move = player.freeMove;
	if (!move.active || move.waypointCount == 0)
		return player.position.tile;
	return TileOf(move.waypointX[move.waypointCount - 1], move.waypointY[move.waypointCount - 1]);
}

Displacement GlideCorrection(const Player &player)
{
	D2_PROBE(d2_GlideCorrection);
	if (!MovementEnabled())
		return {};
	const FreeMoveState &move = player.freeMove;
	if (!move.valid || TileOf(move.x, move.y) != player.position.tile)
		return {};
	const int32_t dx = move.x - player.position.tile.x * SubTile;
	const int32_t dy = move.y - player.position.tile.y * SubTile;
	// One tile is 64 pixels wide and 32 pixels high on screen
	return { (dx - dy) * 32 / SubTile, (dx + dy) * 16 / SubTile };
}

void SetCursorFine(int32_t x, int32_t y)
{
	D2_PROBE(d2_SetCursorFine);
	CursorFineX = x;
	CursorFineY = y;
}

void SendWalkToCursor(bool force)
{
	D2_PROBE(d2_SendWalkToCursor);
	const Point tile = TileOf(CursorFineX, CursorFineY);
	if (tile != cursPosition) {
		// The cursor code snapped to something else (a trigger, the edge of the map, ...): aim at its centre
		CursorFineX = cursPosition.x * SubTile;
		CursorFineY = cursPosition.y * SubTile;
	}
	if (!force && std::abs(CursorFineX - LastSentFineX) < SubTile / 8 && std::abs(CursorFineY - LastSentFineY) < SubTile / 8) {
		// Same target as before: only resend if the hero gave up on the way (e.g. was blocked)
		const FreeMoveState &move = MyPlayer->freeMove;
		const bool arrived = std::abs(move.x - LastSentFineX) < SubTile / 8 && std::abs(move.y - LastSentFineY) < SubTile / 8;
		if (move.active || arrived)
			return;
	}
	LastSentFineX = CursorFineX;
	LastSentFineY = CursorFineY;

	const int fineX = CursorFineX - cursPosition.x * SubTile + SubTile / 2;
	const int fineY = CursorFineY - cursPosition.y * SubTile + SubTile / 2;
	const uint16_t packed = static_cast<uint16_t>(clamp(fineX, 0, 255) | (clamp(fineY, 0, 255) << 8));
	NetSendCmdLocParam1(true, CMD_WALKXY_FINE, cursPosition, packed);
}

void OnWalkFine(Player &player, Point tile, uint16_t packedFine)
{
	D2_PROBE(d2_OnWalkFine);
	if (!MovementEnabled())
		return;
	const int fineX = static_cast<int>(packedFine & 0xFF) - SubTile / 2;
	const int fineY = static_cast<int>(packedFine >> 8) - SubTile / 2;
	ClrPlrPath(player);
	FreeMoveSetTarget(player, tile, fineX, fineY, true);
	player.destAction = ACTION_NONE;
}

int ChanceToHit(int attackRating, int defense, int attackerLevel, int defenderLevel)
{
	D2_PROBE(d2_ChanceToHit);
	attackRating = std::max(attackRating * AttackRatingScale, 1);
	defense = std::max(defense, 0);
	attackerLevel = std::max(attackerLevel, 1);
	defenderLevel = std::max(defenderLevel, 1);

	// 200 * AR / (AR + DEF) * alvl / (alvl + dlvl), done in 64 bit to avoid overflow
	int64_t numerator = 200LL * attackRating * attackerLevel;
	int64_t denominator = static_cast<int64_t>(attackRating + defense) * (attackerLevel + defenderLevel);
	return static_cast<int>(numerator / denominator);
}

int ClampChanceToHit(int chance)
{
	D2_PROBE_FN();
	return clamp(chance, MinChanceToHit, MaxChanceToHit);
}

int PlayerMeleeChanceToHit(const Player &player, const Monster &monster)
{
	D2_PROBE_FN();
	return ChanceToHit(
	    player.GetMeleePiercingToHit(),
	    player.CalculateArmorPierce(monster.armorClass, true),
	    player._pLevel,
	    monster.level(sgGameInitInfo.nDifficulty));
}

int PlayerRangedChanceToHit(const Player &player, const Monster &monster)
{
	D2_PROBE_FN();
	return ChanceToHit(
	    player.GetRangedPiercingToHit(),
	    player.CalculateArmorPierce(monster.armorClass, false),
	    player._pLevel,
	    monster.level(sgGameInitInfo.nDifficulty));
}

int MonsterChanceToHit(const Monster &monster, const Player &player, int monsterToHit, int armor)
{
	D2_PROBE_FN();
	return ChanceToHit(
	    monsterToHit + MonsterBaseAttackRating,
	    armor,
	    monster.level(sgGameInitInfo.nDifficulty),
	    player._pLevel);
}

int PlayerVsPlayerChanceToHit(const Player &attacker, const Player &target, bool ranged)
{
	D2_PROBE_FN();
	return ChanceToHit(
	    ranged ? attacker.GetRangedToHit() : attacker.GetMeleeToHit(),
	    target.GetArmor(),
	    attacker._pLevel,
	    target._pLevel);
}

bool PlayerFlinches(const Player &player, int dam)
{
	D2_PROBE(d2_PlayerFlinches);
	if (dam <= 0)
		return false;
	int divisor = player._pClass == HeroClass::Barbarian ? BarbarianHitRecoveryDivisor : PlayerHitRecoveryDivisor;
	return dam >= player._pMaxHP / divisor;
}

bool MonsterFlinches(const Monster &monster, int dam)
{
	D2_PROBE(d2_MonsterFlinches);
	if (dam <= 0)
		return false;
	return dam >= monster.maxHitPoints / MonsterHitRecoveryDivisor;
}

void ToggleRun()
{
	D2_PROBE(d2_ToggleRun);
	if (!MovementEnabled())
		return;
	RunToggled = !RunToggled;
}

void UpdateLocalRunState()
{
	D2_PROBE(d2_UpdateLocalRunState);
	if (!MovementEnabled() || MyPlayer == nullptr)
		return;

	bool wantsToRun = RunToggled;
	if ((SDL_GetModState() & KMOD_LCTRL) != 0)
		wantsToRun = !wantsToRun;

	if (static_cast<int>(wantsToRun) == LastSentRunState)
		return;

	LastSentRunState = wantsToRun ? 1 : 0;
	NetSendCmdParam1(true, CMD_SETRUN, LastSentRunState);
}

void ShareRunState()
{
	D2_PROBE(d2_ShareRunState);
	LastSentRunState = -1;
}

} // namespace d2

uint32_t D2ModGameId(uint32_t stockGameId)
{
	D2_PROBE_FN();
	if (!d2::MovementEnabled())
		return stockGameId;
	// Replace the last letter with '2' (DRTL -> DRT2, HSHR -> HSH2)
	return (stockGameId & 0xFFFFFF00U) | static_cast<uint32_t>('2');
}

} // namespace devilution
