/**
 * @file d2mod.cpp
 *
 * Diablo 2 style movement and combat rules layered on top of the Diablo 1 engine.
 */
#include "d2mod.h"

#include <algorithm>
#include <cstdlib>
#include <climits>
#include <cstdint>

#include <SDL.h>

#include "engine.h"
#include "cursor.h"
#include "engine/animationinfo.h"
#include "levels/gendung.h"
#include "lighting.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
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
	return StepCodeByDisplacement[3 * (to.y - from.y) + 4 + (to.x - from.x)];
}

/** @brief Integer division rounding half away from zero, deterministic on every client. */
int RoundedDiv(int numerator, int denominator)
{
	if (numerator >= 0)
		return (2 * numerator + denominator) / (2 * denominator);
	return -((-2 * numerator + denominator) / (2 * denominator));
}

/** @brief Point @p i of @p n along the straight line from @p a to @p b. */
Point PointOnLine(Point a, Point b, int i, int n)
{
	return { a.x + RoundedDiv(i * (b.x - a.x), n), a.y + RoundedDiv(i * (b.y - a.y), n) };
}

bool IsLineWalkable(tl::function_ref<bool(Point)> posOk, Point a, Point b)
{
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

} // namespace

Displacement WalkStepDisplacement(int8_t step)
{
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
	return static_cast<int>((v + SubTile / 2) >> 8);
}

Point TileOf(int32_t x, int32_t y)
{
	return { TileOf(x), TileOf(y) };
}

int64_t IntSqrt(int64_t value)
{
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

/** Nearest of the 8 sprite directions to a screen space vector. */
Direction ScreenFacing(int64_t vx, int64_t vy, Direction fallback)
{
	if (vx == 0 && vy == 0)
		return fallback;
	struct Candidate {
		Direction dir;
		int64_t ux;
		int64_t uy;
	};
	// Unit vectors (x1000) of each sprite direction as drawn on screen
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
	Direction best = fallback;
	int64_t bestScore = INT64_MIN;
	for (const Candidate &candidate : Candidates) {
		const int64_t score = candidate.ux * vx + candidate.uy * vy;
		if (score > bestScore) {
			bestScore = score;
			best = candidate.dir;
		}
	}
	return best;
}

/** Facing for moving by a world (tile space) vector. */
Direction WorldFacing(int64_t dx, int64_t dy, Direction fallback)
{
	// Same projection the renderer uses: one tile is 64 pixels wide and 32 pixels high
	return ScreenFacing((dx - dy) * 32, (dx + dy) * 16, fallback);
}

/** Can the hero step from tile @p from into the neighbouring tile @p to? */
bool CanCross(const Player &player, Point from, Point to)
{
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
	const int64_t dx = toX - fromX;
	const int64_t dy = toY - fromY;
	const int64_t length = IntSqrt(dx * dx + dy * dy);
	// Sample every quarter tile; diagonal tile changes are checked with the corner rule
	const int64_t samples = std::max<int64_t>(1, length / (SubTile / 4));
	Point previous = TileOf(fromX, fromY);
	for (int64_t i = 1; i <= samples; i++) {
		const Point tile = TileOf(static_cast<int32_t>(fromX + dx * i / samples), static_cast<int32_t>(fromY + dy * i / samples));
		if (tile == previous)
			continue;
		if (std::abs(tile.x - previous.x) > 1 || std::abs(tile.y - previous.y) > 1)
			return false;
		if (!CanCross(player, previous, tile))
			return false;
		previous = tile;
	}
	return true;
}

/** Put the hero's sub-tile position back on their tile if something else moved them. */
void EnsurePosition(Player &player)
{
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
	return player.isRunning || (leveltype == DTYPE_TOWN && sgGameInitInfo.bRunInTown != 0);
}

void UpdateLightOffset(const Player &player)
{
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
	FreeMoveState &move = player.freeMove;
	if (move.animating && facing == move.facing)
		return;
	move.facing = facing;
	player._pdir = facing;
	if (!move.animating) {
		NewPlrAnim(player, player_graphic::Walk, facing);
		move.animating = true;
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

void FinishMoving(Player &player)
{
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
	EnsurePosition(player);
	FreeMoveState &move = player.freeMove;
	const Point start = player.position.tile;
	fineX = clamp(fineX, -SubTile / 2 + 1, SubTile / 2 - 1);
	fineY = clamp(fineY, -SubTile / 2 + 1, SubTile / 2 - 1);
	const int32_t targetX = tile.x * SubTile + fineX;
	const int32_t targetY = tile.y * SubTile + fineY;

	move.waypointCount = 0;
	move.waypointIndex = 0;

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

	int64_t budget = IsRunning(player) ? RunSpeed : WalkSpeed;
	while (budget > 0 && move.active) {
		const int32_t wx = move.waypointX[move.waypointIndex];
		const int32_t wy = move.waypointY[move.waypointIndex];
		const int64_t dx = wx - move.x;
		const int64_t dy = wy - move.y;
		const int64_t distance = IntSqrt(dx * dx + dy * dy);
		if (distance > 0)
			SetFacing(player, WorldFacing(dx, dy, move.facing));

		int32_t nextX;
		int32_t nextY;
		if (distance <= budget) {
			nextX = wx;
			nextY = wy;
			budget -= distance;
		} else {
			nextX = move.x + static_cast<int32_t>(dx * budget / distance);
			nextY = move.y + static_cast<int32_t>(dy * budget / distance);
			budget = 0;
		}

		if (!MoveTo(player, nextX, nextY)) {
			// Something stepped into the way
			FinishMoving(player);
			return;
		}

		if (move.x == wx && move.y == wy) {
			move.waypointIndex++;
			if (move.waypointIndex >= move.waypointCount)
				move.active = false;
		}
	}

	if (!move.active) {
		FinishMoving(player);
		return;
	}

	// The walk cycle is timed for walking speed; play it faster while running
	if (IsRunning(player)) {
		move.extraFrame = !move.extraFrame;
		if (move.extraFrame)
			player.AnimInfo.processAnimation();
	}
}

bool FreeMoveActive(const Player &player)
{
	return player.freeMove.active;
}

void FreeMoveStop(Player &player)
{
	// The walk animation is replaced by whatever the hero does next, or by standing on the next tick
	player.freeMove.active = false;
}

void FreeMoveReset(Player &player)
{
	player.freeMove = {};
}

Point FreeMoveTargetTile(const Player &player)
{
	const FreeMoveState &move = player.freeMove;
	if (!move.active || move.waypointCount == 0)
		return player.position.tile;
	return TileOf(move.waypointX[move.waypointCount - 1], move.waypointY[move.waypointCount - 1]);
}

Displacement GlideCorrection(const Player &player)
{
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
	CursorFineX = x;
	CursorFineY = y;
}

void SendWalkToCursor(bool force)
{
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
	const int fineX = static_cast<int>(packedFine & 0xFF) - SubTile / 2;
	const int fineY = static_cast<int>(packedFine >> 8) - SubTile / 2;
	ClrPlrPath(player);
	FreeMoveSetTarget(player, tile, fineX, fineY, true);
	player.destAction = ACTION_NONE;
}

int ChanceToHit(int attackRating, int defense, int attackerLevel, int defenderLevel)
{
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
	return clamp(chance, MinChanceToHit, MaxChanceToHit);
}

int PlayerMeleeChanceToHit(const Player &player, const Monster &monster)
{
	return ChanceToHit(
	    player.GetMeleePiercingToHit(),
	    player.CalculateArmorPierce(monster.armorClass, true),
	    player._pLevel,
	    monster.level(sgGameInitInfo.nDifficulty));
}

int PlayerRangedChanceToHit(const Player &player, const Monster &monster)
{
	return ChanceToHit(
	    player.GetRangedPiercingToHit(),
	    player.CalculateArmorPierce(monster.armorClass, false),
	    player._pLevel,
	    monster.level(sgGameInitInfo.nDifficulty));
}

int MonsterChanceToHit(const Monster &monster, const Player &player, int monsterToHit, int armor)
{
	return ChanceToHit(
	    monsterToHit + MonsterBaseAttackRating,
	    armor,
	    monster.level(sgGameInitInfo.nDifficulty),
	    player._pLevel);
}

int PlayerVsPlayerChanceToHit(const Player &attacker, const Player &target, bool ranged)
{
	return ChanceToHit(
	    ranged ? attacker.GetRangedToHit() : attacker.GetMeleeToHit(),
	    target.GetArmor(),
	    attacker._pLevel,
	    target._pLevel);
}

bool PlayerFlinches(const Player &player, int dam)
{
	if (dam <= 0)
		return false;
	int divisor = player._pClass == HeroClass::Barbarian ? BarbarianHitRecoveryDivisor : PlayerHitRecoveryDivisor;
	return dam >= player._pMaxHP / divisor;
}

bool MonsterFlinches(const Monster &monster, int dam)
{
	if (dam <= 0)
		return false;
	return dam >= monster.maxHitPoints / MonsterHitRecoveryDivisor;
}

void ToggleRun()
{
	RunToggled = !RunToggled;
}

void UpdateLocalRunState()
{
	if (MyPlayer == nullptr)
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
	LastSentRunState = -1;
}

} // namespace d2

} // namespace devilution
