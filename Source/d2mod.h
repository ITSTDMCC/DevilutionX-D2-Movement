/**
 * @file d2mod.h
 *
 * Diablo 2 style movement and combat rules layered on top of the Diablo 1 engine.
 * All tuning values for the mod live here.
 */
#pragma once

#include <cstdint>

#include <function_ref.hpp>

#include "engine/direction.hpp"
#include "engine/path.h"
#include "engine/point.hpp"

namespace devilution {

struct Player;
struct Monster;

namespace d2 {

/** Sub-tile units per tile for free movement positions. */
constexpr int32_t SubTile = 256;
/** Walking speed in sub-tile units per game tick (one tile in 8 ticks, the same as Diablo 1 walking). */
constexpr int32_t WalkSpeed = 32;
/** Running speed in sub-tile units per game tick. */
constexpr int32_t RunSpeed = 48;
constexpr int MaxMoveWaypoints = MaxPathLength + 1;

/**
 * Diablo 2 style free movement. Heroes have a position finer than a tile and move in a straight
 * line towards an exact point at any angle. The tile they stand in is still what the rest of the game
 * (monsters, collision, saving) sees: it is updated the moment the hero crosses into a new tile.
 * Positions are world tile coordinates times SubTile, so tile centres are multiples of SubTile.
 */
struct FreeMoveState {
	/** x and y hold a real position (otherwise they get reset to the hero's tile centre). */
	bool valid = false;
	/** The hero is currently moving. */
	bool active = false;
	/** The walk animation is playing because of free movement. */
	bool animating = false;
	/** Alternating flag used to speed the walk animation up while running. */
	bool extraFrame = false;
	int32_t x = 0;
	int32_t y = 0;
	uint8_t waypointCount = 0;
	uint8_t waypointIndex = 0;
	int32_t waypointX[MaxMoveWaypoints] = {};
	int32_t waypointY[MaxMoveWaypoints] = {};
	Direction facing = Direction::South;
};

/** Running is on by default, like toggling run on in Diablo 2. */
constexpr bool RunByDefault = true;

/** A player flinches when a single hit deals at least 1/N of their max life (Diablo 2 uses 12). */
constexpr int PlayerHitRecoveryDivisor = 12;
/** Barbarians keep a sturdier threshold than other classes, as they do in Diablo 1. */
constexpr int BarbarianHitRecoveryDivisor = 10;
/** A monster flinches when a single hit deals at least 1/N of its max life. */
constexpr int MonsterHitRecoveryDivisor = 8;

/**
 * Attack ratings are the Diablo 1 to-hit values multiplied by this.
 * Raise it to make hitting easier against high armor targets.
 */
constexpr int AttackRatingScale = 1;
/** Monster attack rating is its Diablo 1 to-hit value plus this (Diablo 1 adds the same base). */
constexpr int MonsterBaseAttackRating = 30;

constexpr int MinChanceToHit = 5;
constexpr int MaxChanceToHit = 95;

/**
 * @brief Diablo 2 chance to hit: 200% * AR / (AR + DEF) * alvl / (alvl + dlvl), clamped to 5..95.
 * @return Chance in percent, not yet clamped so callers can apply extra modifiers first.
 */
int ChanceToHit(int attackRating, int defense, int attackerLevel, int defenderLevel);

/** @brief Clamp a hit chance to the Diablo 2 limits. */
int ClampChanceToHit(int chance);

/** @brief Player melee chance to hit a monster, before adjacent (cleave) penalties. */
int PlayerMeleeChanceToHit(const Player &player, const Monster &monster);

/** @brief Player arrow chance to hit a monster. Diablo 2 has no range penalty. */
int PlayerRangedChanceToHit(const Player &player, const Monster &monster);

/** @brief Monster physical chance to hit a player. @p armor already includes situational bonuses. */
int MonsterChanceToHit(const Monster &monster, const Player &player, int monsterToHit, int armor);

/** @brief Player versus player physical chance to hit. */
int PlayerVsPlayerChanceToHit(const Player &attacker, const Player &target, bool ranged);

/** @brief Should this hit put the player into hit recovery? @p dam is in 1/64 HP units. */
bool PlayerFlinches(const Player &player, int dam);

/** @brief Should this hit put the monster into hit recovery? @p dam is in 1/64 HP units. */
bool MonsterFlinches(const Monster &monster, int dam);

/**
 * @brief Replace stretches of an A* path with straight lines where nothing blocks them, so the hero
 * moves along the true line to the target (mixing the two nearest of the 8 step directions) instead of
 * doglegging diagonally and then straight.
 * @param posOk Same check the path finder used for each tile
 * @param start Tile the path starts from
 * @param path Step codes (WALK_*), rewritten in place
 * @param length Number of steps in @p path
 * @param segmentLengths Optional output: at the first step of each straight segment its length in steps, otherwise 0
 * @return New number of steps (never more than @p length)
 */
int StraightenPath(tl::function_ref<bool(Point)> posOk, Point start, int8_t path[MaxPathLength], int length, uint8_t segmentLengths[MaxPathLength] = nullptr);

/** @brief Tile displacement for a WALK_* step code. */
Displacement WalkStepDisplacement(int8_t step);

/**
 * @brief Send the hero towards a point. Uses a straight line when nothing blocks it, otherwise the
 * Diablo 1 path finder with the result straightened into as few lines as possible.
 * @param tile Target tile
 * @param fineX Sub-tile offset from the target tile's centre (-127..127), x axis
 * @param fineY Sub-tile offset from the target tile's centre (-127..127), y axis
 * @param endspace False to stop next to the target tile instead of on it (attacking, picking up, ...)
 */
void FreeMoveSetTarget(Player &player, Point tile, int fineX, int fineY, bool endspace);

/** @brief Advance free movement by one game tick. Call while the hero is in PM_STAND. */
void FreeMoveTick(Player &player);

/** @brief Is the hero currently moving? */
bool FreeMoveActive(const Player &player);

/** @brief Stop moving where the hero is (does not snap back to the tile centre). */
void FreeMoveStop(Player &player);

/** @brief Forget the sub-tile position, e.g. on level change or teleport. */
void FreeMoveReset(Player &player);

/** @brief Tile the hero is heading for. */
Point FreeMoveTargetTile(const Player &player);

/** @brief Screen offset from the hero's tile centre to where they really are. */
Displacement GlideCorrection(const Player &player);

/** @brief Remember the exact sub-tile point under the mouse (set by the cursor code). */
void SetCursorFine(int32_t x, int32_t y);

/**
 * @brief Tell the game to move the local hero to the exact point under the mouse.
 * @param force Send even if the target barely changed since the last time
 */
void SendWalkToCursor(bool force);

/** @brief Apply a fine walk command (CMD_WALKXY_FINE). */
void OnWalkFine(Player &player, Point tile, uint16_t packedFine);

/** @brief Toggle between walking and running (Diablo 2's R key). */
void ToggleRun();

/**
 * @brief Work out whether the local player wants to run (toggle, inverted while Left Ctrl is held)
 * and tell everyone in the game when that changes. Called once per game tick.
 */
void UpdateLocalRunState();

/** @brief Re-announce the local run state, e.g. when another player joins the level. */
void ShareRunState();

} // namespace d2

} // namespace devilution
