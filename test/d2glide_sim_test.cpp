#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "d2mod.h"
#include "engine/render/scrollrt.h"
#include "levels/gendung.h"
#include "lighting.h"
#include "player.h"

namespace devilution {
namespace {

Displacement TileScreen(Point tile)
{
	return { (tile.x - tile.y) * 32, (tile.x + tile.y) * 16 };
}

/** Where the renderer would draw the hero this tick, in screen pixels. */
Displacement DrawnPosition(const Player &player)
{
	// Sideways walks are drawn from the destination tile (that is where dPlayer is positive)
	Displacement pos = TileScreen(player._pmode == PM_WALK_SIDEWAYS ? player.position.future : player.position.tile);
	if (player.isWalking())
		pos += GetOffsetForWalking(player.AnimInfo, player._pdir);
	return pos + d2::GlideCorrection(player);
}

double DistanceFromLine(Displacement p, Displacement a, Displacement b)
{
	const double dx = b.deltaX - a.deltaX;
	const double dy = b.deltaY - a.deltaY;
	const double len = std::sqrt(dx * dx + dy * dy);
	return std::abs(dx * (a.deltaY - p.deltaY) - dy * (a.deltaX - p.deltaX)) / len;
}

TEST(D2GlideSim, HeroIsDrawnOnTheStraightLine)
{
	Players.resize(1);
	MyPlayerId = 0;
	MyPlayer = &Players[0];
	Player &player = Players[0];
	player = {};
	player.plractive = true;
	player.plrlevel = 0;
	currlevel = 0;
	leveltype = DTYPE_TOWN;
	player._pMaxHP = player._pHitPoints = 100 << 6;
	player._pMaxHPBase = player._pHPBase = 100 << 6;
	player._pLevel = 1;
	player._pNextExper = 2000;
	player._pWFrames = 8;
	player._pNFrames = 20;
	player.isRunning = false;
	player.lightId = NO_LIGHT;
	player.destAction = ACTION_NONE;

	double worstOverall = 0;
	int walkTicks = 0;
	for (int running = 0; running < 2; running++) {
		for (int hold = 0; hold < 2; hold++) {
			for (int dx = -6; dx <= 6; dx++) {
				for (int dy = -6; dy <= 6; dy++) {
					for (int fine = 0; fine < 2; fine++) {
						const Point start { 40, 40 };
						const Point target = start + Displacement { dx, dy };
						// Aim at an off-centre point inside the target tile half of the time
						const int fineX = fine != 0 ? 90 : 0;
						const int fineY = fine != 0 ? -60 : 0;
						player.isRunning = running != 0;
						player.position.tile = player.position.future = player.position.old = start;
						d2::FreeMoveReset(player);
						StartStand(player, Direction::South);
						d2::FreeMoveSetTarget(player, target, fineX, fineY, true);

						const Displacement a = TileScreen(start);
						const Displacement b = TileScreen(target) + Displacement { (fineX - fineY) * 32 / 256, (fineX + fineY) * 16 / 256 };
						double worst = 0;
						int tick = 0;
						for (; tick < 300 && d2::FreeMoveActive(player); tick++) {
							if (hold != 0)
								d2::FreeMoveSetTarget(player, target, fineX, fineY, true);
							ProcessPlayers();
							if (a != b)
								worst = std::max(worst, DistanceFromLine(DrawnPosition(player), a, b));
						}
						if (running == 0 && hold == 0 && fine == 0 && dx == 6 && dy == 0)
							walkTicks = tick;
						EXPECT_EQ(player.position.tile, target) << "run " << running << " hold " << hold << " fine " << fine << " d " << dx << "," << dy << " ticks " << tick;
						const Displacement end = DrawnPosition(player);
						EXPECT_LE(std::abs(end.deltaX - b.deltaX) + std::abs(end.deltaY - b.deltaY), 2) << "Did not stop on the exact point";
						EXPECT_EQ(player._pmode, PM_STAND);
						if (worst > 3.5)
							std::printf("run %d hold %d fine %d target (%d,%d): strays %.1f px\n", running, hold, fine, dx, dy, worst);
						worstOverall = std::max(worstOverall, worst);
					}
				}
			}
		}
	}
	EXPECT_LE(worstOverall, 3.5) << "Hero strays from the straight line";
	// Six tiles along a tile axis at Diablo 1 walking speed: 8 ticks a tile
	EXPECT_NEAR(walkTicks, 48, 1);
}

} // namespace
} // namespace devilution
