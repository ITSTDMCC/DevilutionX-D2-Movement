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
#include "engine/animationinfo.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "player.h"
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
	if (length < 3) {
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

constexpr int32_t Fixed = 256;

/** Screen position of a tile's origin in 1/256 pixels (same projection the renderer uses). */
void TileToScreen(Point tile, int32_t &x, int32_t &y)
{
	x = (tile.x - tile.y) * 32 * Fixed;
	y = (tile.x + tile.y) * 16 * Fixed;
}

/** Visual position after @p progress / @p scale of the glide. */
void GlidePosition(const GlideState &glide, int64_t progress, int64_t scale, int32_t &x, int32_t &y)
{
	int32_t toX;
	int32_t toY;
	TileToScreen(glide.toTile, toX, toY);
	x = glide.fromX + static_cast<int32_t>((toX - glide.fromX) * progress / scale);
	y = glide.fromY + static_cast<int32_t>((toY - glide.fromY) * progress / scale);
}

/** Nearest of the 8 sprite directions to a screen space vector. */
Direction ScreenFacing(int32_t vx, int32_t vy, Direction fallback)
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

void FinishGlideStep(GlideState &glide)
{
	if (glide.active && glide.inStep) {
		glide.stepsDone++;
		glide.inStep = false;
	}
}

} // namespace

Direction GlideStartStep(Player &player)
{
	GlideState &glide = player.glide;
	FinishGlideStep(glide);

	const Point stepStart = player.position.tile;
	const bool continues = glide.active && glide.stepsDone < glide.totalSteps && player.walkSegLen[0] == 0;
	if (!continues) {
		// Start the new straight segment from wherever the hero is drawn right now, so nothing jumps
		int32_t fromX;
		int32_t fromY;
		if (glide.active) {
			GlidePosition(glide, glide.stepsDone, std::max<int>(glide.totalSteps, 1), fromX, fromY);
		} else {
			TileToScreen(stepStart, fromX, fromY);
			fromX += glide.residualX;
			fromY += glide.residualY;
		}

		const int length = std::max<int>(player.walkSegLen[0], 1);
		Point toTile = stepStart;
		for (int i = 0; i < length && i < static_cast<int>(MaxPathLength) && player.walkpath[i] != WALK_NONE; i++)
			toTile += WalkStepDisplacement(player.walkpath[i]);

		int32_t startX;
		int32_t startY;
		int32_t toX;
		int32_t toY;
		TileToScreen(stepStart, startX, startY);
		TileToScreen(toTile, toX, toY);

		glide.active = true;
		glide.totalSteps = static_cast<uint8_t>(length);
		glide.stepsDone = 0;
		glide.fromX = fromX;
		glide.fromY = fromY;
		glide.toTile = toTile;
		glide.residualX = 0;
		glide.residualY = 0;
		// Facing only depends on tiles, so it is identical on every client
		glide.facing = ScreenFacing(toX - startX, toY - startY, player._pdir);
	}

	int32_t startX;
	int32_t startY;
	int32_t endX;
	int32_t endY;
	TileToScreen(stepStart, startX, startY);
	TileToScreen(stepStart + WalkStepDisplacement(player.walkpath[0]), endX, endY);
	glide.inStep = true;
	glide.stepStart = stepStart;
	glide.stepVecX = endX - startX;
	glide.stepVecY = endY - startY;
	return glide.facing;
}

void GlideCancelStep(Player &player)
{
	player.glide.inStep = false;
}

void GlideTick(Player &player)
{
	GlideState &glide = player.glide;
	if (player.isWalking())
		return;

	if (glide.active) {
		FinishGlideStep(glide);
		int32_t x;
		int32_t y;
		GlidePosition(glide, glide.stepsDone, std::max<int>(glide.totalSteps, 1), x, y);
		int32_t tileX;
		int32_t tileY;
		TileToScreen(player.position.tile, tileX, tileY);
		glide.residualX = x - tileX;
		glide.residualY = y - tileY;
		glide.active = false;
		// Anything bigger than a tile means the hero was moved some other way (teleport, level change)
		if (std::abs(glide.residualX) > 64 * Fixed || std::abs(glide.residualY) > 32 * Fixed) {
			glide.residualX = 0;
			glide.residualY = 0;
		}
		return;
	}

	// Ease leftovers back onto the tile over a few ticks
	glide.residualX = glide.residualX * 2 / 3;
	glide.residualY = glide.residualY * 2 / 3;
	if (std::abs(glide.residualX) < Fixed / 2)
		glide.residualX = 0;
	if (std::abs(glide.residualY) < Fixed / 2)
		glide.residualY = 0;
}

void GlideReset(Player &player)
{
	player.glide = {};
}

Displacement GlideCorrection(const Player &player)
{
	const GlideState &glide = player.glide;
	if (!glide.active || !glide.inStep || !player.isWalking())
		return { glide.residualX / Fixed, glide.residualY / Fixed };

	// Where the engine draws the hero: the step's start tile plus progress along this step
	const int64_t base = AnimationInfo::baseValueFraction;
	const int64_t progress = std::min<int64_t>(player.AnimInfo.getAnimationProgress(), base);
	int32_t engineX;
	int32_t engineY;
	TileToScreen(glide.stepStart, engineX, engineY);
	engineX += static_cast<int32_t>(glide.stepVecX * progress / base);
	engineY += static_cast<int32_t>(glide.stepVecY * progress / base);

	// Where we want the hero: the same fraction of the way along the straight segment
	int32_t glideX;
	int32_t glideY;
	GlidePosition(glide, glide.stepsDone * base + progress, std::max<int64_t>(glide.totalSteps, 1) * base, glideX, glideY);

	return { (glideX - engineX) / Fixed, (glideY - engineY) / Fixed };
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
