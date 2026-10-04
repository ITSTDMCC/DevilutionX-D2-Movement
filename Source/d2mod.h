/**
 * @file d2mod.h
 *
 * Diablo 2 style movement and combat rules layered on top of the Diablo 1 engine.
 * All tuning values for the mod live here.
 *
 * Part of DevilutionX-D2-Movement. Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
 * Licensed under the Sustainable Use License (LICENSE.md); see NOTICE-D2MOVEMENT.md.
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
struct Missile;

namespace d2 {

/** @brief Is Diablo 2 movement on? (Gameplay option, fixed for the length of a game.) Off means stock Diablo 1. */
bool MovementEnabled();

/** @brief Are the Diablo 2 to-hit and hit recovery rules on? Off means stock Diablo 1 combat. */
bool CombatEnabled();

/** Sub-tile units per tile for free movement positions. */
constexpr int32_t SubTile = 256;

/** Diablo 2 charstats.txt WalkVelocity and RunVelocity (the same for every class). */
constexpr int32_t D2WalkVelocity = 6;
constexpr int32_t D2RunVelocity = 9;
/** Diablo 2 path speed: PATH_SetVelocity(velocity << 8), each frame moves (base 1024 * velocity) >> 6 units of 1/65536 subtile. */
constexpr int32_t D2UnitsPerFrame(int32_t velocity)
{
	return (1024 * (velocity << 8)) >> 6;
}
constexpr int32_t D2FramesPerSecond = 25;
constexpr int32_t D1TicksPerSecond = 20;
/**
 * Both games draw a 640x480 view; a Diablo 2 subtile is 32x16 pixels and a Diablo 1 tile 64x32,
 * so one Diablo 1 tile is two Diablo 2 subtiles on screen. Converts a D2 velocity to sub-tile units per D1 tick.
 */
constexpr int32_t D2SpeedPerTick(int32_t velocity)
{
	return static_cast<int32_t>(static_cast<int64_t>(D2UnitsPerFrame(velocity)) * D2FramesPerSecond * (SubTile / 2) / (65536LL * D1TicksPerSecond));
}
/** Diablo 2's own walk and run speeds on the Diablo 1 grid (4.69 and 7.03 tiles a second), kept for reference. */
constexpr int32_t D2WalkSpeed = D2SpeedPerTick(D2WalkVelocity);
constexpr int32_t D2RunSpeed = D2SpeedPerTick(D2RunVelocity);
static_assert(D2WalkSpeed == 60 && D2RunSpeed == 90, "Diablo 2 speeds do not convert exactly");

/**
 * Pacing follows Diablo 1 (players found Diablo 2 speeds too fast for Diablo 1's dungeons): one tile per
 * 8 ticks, measured like Diablo 1 counts steps (the larger of the x and y distance), so every one of the
 * 8 directions takes exactly as long as a stock step and directions in between blend smoothly.
 */
constexpr int32_t D1WalkSpeed = SubTile / 8;
/** Running exists only in town, at the speed of the stock "Run in Town" jog. */
constexpr int32_t D1TownRunSpeed = 2 * D1WalkSpeed;
/** Speed the Diablo 1 walk cycle is drawn for (one tile per 8 frame stride): faster movement plays it faster. */
constexpr int32_t D1WalkCycleSpeed = SubTile / 8;

/** @brief Speed in sub-tile units per tick (larger axis) the hero moves at right now. */
int32_t MoveSpeed(const Player &player);

/** Diablo 2 direction: unit vector scaled to 4096 and one of 64 directions (0 is +x+y, counting towards +y). */
struct D2Heading {
	int32_t x;
	int32_t y;
	int dir64;
};

/** @brief Port of D2Common PATH_GetDirectionVector: heading from a world delta using Diablo 2's 128 entry tangent table. */
D2Heading D2DirectionVector(int64_t dx, int64_t dy);

/** @brief Entry @p tangent (0..127) of the Diablo 2 tangent table, for checking against D2Common.dll. */
D2Heading D2TangentTableEntry(int tangent);

/** @brief Diablo 1 sprite direction for a Diablo 2 64 step direction (players have 8 directions in both games). */
Direction FacingFromDir64(int dir64);

/**
 * @brief Sprite direction for moving by a world delta: whichever of the 8 Diablo 1 walk sprites points closest to
 * the motion as it appears on screen. Keeps @p current unless another sprite is clearly closer, so a hero moving
 * along a boundary between two sprites does not flicker.
 */
Direction FacingForMotion(int64_t dx, int64_t dy, Direction current);
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
	/** Walk cycle progress carried between ticks, in sub-tile units (see D1WalkCycleSpeed). */
	int32_t animCarry = 0;
	/** Fraction of a sub-tile unit not yet moved, in 1/4096ths (Diablo 2 keeps 1/65536 subtile precision). */
	int32_t carryX = 0;
	int32_t carryY = 0;
	/** Waypoints before this index belong to a corner cut (not where the hero is really heading). */
	uint8_t hopEnd = 0;
	/** Distance a corner cut overspent last tick, taken off this tick's budget. */
	int32_t debt = 0;
	/** Where the current order goes, so the hero can find a new way when the old one turns out to be blocked. */
	int32_t goalX = 0;
	int32_t goalY = 0;
	bool goalEndspace = true;
	uint8_t repaths = 0;
	/** Safety net: an order is still unfinished (the hero should be getting somewhere). */
	bool wantGoal = false;
	/** Ticks in a row the hero wanted to move but did not. */
	uint8_t idleTicks = 0;
	/** Ticks since the hero last got closer to the goal (catches going back and forth), and the closest yet. */
	uint8_t noImproveTicks = 0;
	int32_t bestGoalDistance = INT32_MAX;
	int32_t progressX = 0;
	int32_t progressY = 0;
	/** Safety net engaged: the rest of this order is walked by stock Diablo 1 tile walking. */
	bool handover = false;
	/** Waiting to reach the tile centre before the stock walk starts. */
	bool handoverPending = false;
	Point handoverGoal;
	uint8_t handoverLength = 0;
	int8_t handoverPath[MaxPathLength] = {};
	/** Last walk frame a footstep was checked on. */
	int8_t lastStepFrame = -1;
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

/** @brief The hero started another action (attack, hit recovery, block, death): stop moving, keep the position. */
void FreeMoveInterrupt(Player &player);

/**
 * @brief Safety net: hand the rest of the current order to stock Diablo 1 tile walking (after stepping to the tile
 * centre). Engaged automatically when the hero wants to move but has not for a few ticks; logs a snapshot to
 * d2movement-stuck.log in the save folder.
 */
void EngageTileWalkFallback(Player &player, const char *reason);

/** @brief Forget the sub-tile position, e.g. on level change or teleport. */
void FreeMoveReset(Player &player);

/** @brief Tile the hero is heading for. */
Point FreeMoveTargetTile(const Player &player);

/**
 * @brief Tile a gamepad push should walk to. Like Diablo 2's analog movement, a push that is blocked (into a wall
 * beside a doorway, past a corner) slides to whichever of the two neighbouring directions is open, so the hero
 * glides through doorways and along walls instead of stopping. Returns false when every way is blocked.
 */
bool GamepadStep(const Player &player, Direction dir, Point &target);

/** @brief Record where a new missile really leaves from / lands on (heroes standing off their tile centre). */
void AnchorMissileVisuals(Missile &missile, Point src, Point dst);

/**
 * @brief Extra screen offset to draw a missile with: the anchors blended over its flight.
 * @param progress Fraction of the way to the next game tick (0..AnimationInfo::baseValueFraction)
 */
Displacement MissileVisualOffset(const Missile &missile, int progress);

/** Ticks a monster hit mid-step takes to ease back to its tile on screen. */
constexpr int8_t MonsterHitSlideTicks = 4;

/**
 * @brief Diablo 2 movement: a monster hit mid-step stops on whichever of its two tiles it is nearer to, like a
 * Diablo 2 unit stopping where it is, instead of always being sent back to the tile it was leaving (stock rule).
 * Call before StartMonsterGotHit. Does nothing in stock mode, for monsters that are not walking, or for knockback.
 */
void SettleMonsterHitMidStep(Monster &monster);

/** @brief Remember where a walking monster was drawn just before a hit sends it back to its old tile. */
void BeginMonsterHitSlide(Monster &monster);

/** @brief Extra screen offset to draw a monster with while it eases back after a hit (zero otherwise). */
Displacement MonsterHitSlideOffset(const Monster &monster, int progress);

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

/** @brief Remember this tick (hero, order, nearby monsters) in a short ring buffer for movement reports. */
void RecordTrace();

/**
 * @brief Write the last few seconds of the trace, plus a map of the surroundings, to
 * d2movement-report-<time>.txt in the save folder (bound to K: "something looked wrong").
 */
void WriteMovementReport();

/**
 * @brief Harness only (environment variable D2_CAPTURE_DIR): after a frame is drawn, save it while a monster is
 * easing back from a hit, so the drawing can be checked frame by frame. Does nothing otherwise.
 */
void AfterFrameDrawn();

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
