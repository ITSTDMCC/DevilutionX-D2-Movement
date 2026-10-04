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

/** How many upcoming steps decide which way the hero faces while walking a straightened path. */
constexpr int WalkFacingLookahead = 8;

/**
 * @brief Replace stretches of an A* path with straight lines where nothing blocks them, so the hero
 * moves along the true line to the target (mixing the two nearest of the 8 step directions) instead of
 * doglegging diagonally and then straight.
 * @param posOk Same check the path finder used for each tile
 * @param start Tile the path starts from
 * @param path Step codes (WALK_*), rewritten in place
 * @param length Number of steps in @p path
 * @return New number of steps (never more than @p length)
 */
int StraightenPath(tl::function_ref<bool(Point)> posOk, Point start, int8_t path[MaxPathLength], int length);

/** @brief Tile displacement for a WALK_* step code. */
Displacement WalkStepDisplacement(int8_t step);

/**
 * @brief Direction the hero's sprite should face while walking: towards a point a few steps ahead,
 * so the sprite holds steady while the steps alternate between two directions.
 */
Direction WalkFacing(const Player &player);

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
