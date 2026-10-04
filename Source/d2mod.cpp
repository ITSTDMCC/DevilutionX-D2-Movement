/**
 * @file d2mod.cpp
 *
 * Diablo 2 style movement and combat rules layered on top of the Diablo 1 engine.
 */
#include "d2mod.h"

#include <algorithm>
#include <cstdint>

#include <SDL.h>

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

} // namespace

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
