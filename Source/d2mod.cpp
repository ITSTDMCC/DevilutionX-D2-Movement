/**
 * @file d2mod.cpp
 *
 * Diablo 2 style movement and combat rules layered on top of the Diablo 1 engine.
 */
#include "d2mod.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>

#include <SDL.h>

#include "engine.h"
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

int StraightenPath(tl::function_ref<bool(Point)> posOk, Point start, int8_t path[MaxPathLength], int length)
{
	if (length < 3)
		return length;

	// Tiles the original path visits, including the start
	Point waypoints[MaxPathLength + 1];
	waypoints[0] = start;
	for (int i = 0; i < length; i++)
		waypoints[i + 1] = waypoints[i] + WalkStepDisplacement(path[i]);

	int8_t straightened[MaxPathLength];
	int newLength = 0;
	int i = 0;
	while (i < length) {
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
		i = j;
	}

	for (int k = 0; k < newLength; k++)
		path[k] = straightened[k];
	return newLength;
}

Direction WalkFacing(const Player &player)
{
	Point target = player.position.tile;
	for (int i = 0; i < WalkFacingLookahead && i < static_cast<int>(MaxPathLength) && player.walkpath[i] != WALK_NONE; i++)
		target += WalkStepDisplacement(player.walkpath[i]);
	if (target == player.position.tile)
		return player._pdir;
	return GetDirection(player.position.tile, target);
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
