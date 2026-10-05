/**
 * Headless harness for the Diablo 2 movement mod: movement parity with the Diablo 2 reference,
 * collision, stock-mode parity and probe coverage. Nothing is drawn; heroes are driven through the
 * real ProcessPlayers loop on a synthetic level, and the per-tick probe invariants run throughout.
 *
 * Environment (set by tools/d2harness/run_harness.ps1):
 *   D2_COMMON_DLL  path to Diablo 2 1.12 D2Common.dll, for checking the tangent table byte for byte
 *
 * Part of DevilutionX-D2-Movement. Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
 * Licensed under the Sustainable Use License (LICENSE.md); see NOTICE-D2MOVEMENT.md.
 */
#include <gtest/gtest.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "d2mod.h"
#include "d2probe.h"
#include "diablo.h"
#include "engine/path.h"
#include "levels/gendung.h"
#include "lighting.h"
#include "multi.h"
#include "missiles.h"
#include "monster.h"
#include "engine/render/scrollrt.h"
#include "objects.h"
#include "options.h"
#include "player.h"
#include "utils/paths.h"

namespace devilution {
namespace {

constexpr Point Start { 40, 40 };

void ClearLevel()
{
	std::memset(dPlayer, 0, sizeof(dPlayer));
	std::memset(dMonster, 0, sizeof(dMonster));
	std::memset(dPiece, 0, sizeof(dPiece));
	std::memset(dObject, 0, sizeof(dObject));
	SOLData.fill(TileProperties::None);
	SOLData[1] = TileProperties::Solid;
	currlevel = 0;
	leveltype = DTYPE_TOWN;
}

void Wall(Point tile)
{
	dPiece[tile.x][tile.y] = 1;
}

Player &SetupHero(Point start, bool running)
{
	Players.resize(2);
	Players[1] = {};
	MyPlayerId = 0;
	MyPlayer = &Players[0];
	Player &player = Players[0];
	player = {};
	player.plractive = true;
	player.plrlevel = 0;
	player._pMaxHP = player._pHitPoints = 100 << 6;
	player._pMaxHPBase = player._pHPBase = 100 << 6;
	player._pLevel = 1;
	player._pNextExper = 2000;
	player._pWFrames = 8;
	player._pNFrames = 20;
	player.isRunning = running;
	player.lightId = NO_LIGHT;
	player.destAction = ACTION_NONE;
	player.position.tile = player.position.future = player.position.old = start;
	dPlayer[start.x][start.y] = 1;
	d2probe::ForgetPositions();
	ClrPlrPath(player); // as InitPlayer does: an empty walk path is WALK_NONE, not zero
	d2::FreeMoveReset(player);
	StartStand(player, Direction::South);
	return player;
}

/** Runs the real game tick until the hero stops; returns ticks used (capped so nothing can spin). */
int RunHero(Player &player, int maxTicks = 600)
{
	int tick = 0;
	for (; tick < maxTicks && d2::FreeMoveActive(player); tick++) {
		ProcessPlayers();
		d2probe::CheckTick();
	}
	ProcessPlayers();
	d2probe::CheckTick();
	return tick;
}

class D2Harness : public ::testing::Test {
protected:
	void SetUp() override
	{
		// Keep the safety net's log out of the player's real save folder
		static const std::string LogDir = [] {
			const char *dir = std::getenv("D2_HARNESS_SAVE_DIR");
			return std::string(dir != nullptr ? dir : ".") + "/";
		}();
		paths::SetPrefPath(LogDir);
		sgOptions.Gameplay.d2Movement.SetValue(true);
		sgOptions.Gameplay.d2Combat.SetValue(false);
		sgOptions.Audio.walkingSound.SetValue(true);
		sgGameInitInfo.bRunInTown = 0;
		d2probe::ForceChecks(true);
		failuresBefore = d2probe::FailureCount();
		ClearLevel();
	}

	void TearDown() override
	{
		EXPECT_EQ(d2probe::FailureCount(), failuresBefore) << "probe invariants failed (see FAIL lines in the probe log)";
		d2probe::ForceChecks(false);
	}

	uint64_t failuresBefore = 0;
};

// ---------------------------------------------------------------------------------------------
// Reference: Diablo 2 movement written straight from D2Common (PATH_GetDirectionVector and the
// per frame step: velocity vector = ((1024 * (velocity << 8)) >> 6) * direction >> 12, 25 frames
// a second, positions in 1/65536 subtile). Kept separate from the mod's port on purpose.

struct RefEntry {
	int32_t x, y, angle;
};

std::vector<RefEntry> ReadD2CommonTable(size_t &offset)
{
	const char *path = std::getenv("D2_COMMON_DLL");
	if (path == nullptr)
		return {};
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return {};
	std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	// Entry 0 is {0, 4096, 0} and entry 1 {32, 4095, 0}: find that run, then read 128 entries
	const int32_t head[6] = { 0, 4096, 0, 32, 4095, 0 };
	for (size_t i = 0; i + 128 * 12 <= bytes.size(); i += 4) {
		if (std::memcmp(bytes.data() + i, head, sizeof(head)) != 0)
			continue;
		std::vector<RefEntry> table(128);
		std::memcpy(table.data(), bytes.data() + i, 128 * 12);
		offset = i;
		return table;
	}
	return {};
}

struct RefHeading {
	int32_t x, y;
	int dir;
};

RefHeading RefDirection(const std::vector<RefEntry> &lut, int64_t sx, int64_t sy, int64_t tx, int64_t ty)
{
	const bool xLess = !(sx > tx);
	int64_t minX = xLess ? sx : tx, maxX = xLess ? tx : sx;
	const bool yLess = !(sy > ty);
	int64_t minY = yLess ? sy : ty, maxY = yLess ? ty : sy;
	bool swapped = false;
	if (maxX - minX > maxY - minY) {
		std::swap(minX, minY);
		std::swap(maxX, maxY);
		swapped = true;
	}
	int tangent = 0;
	if (maxY != minY)
		tangent = static_cast<int>(127 * (maxX - minX) / (maxY - minY));
	const RefEntry &e = lut[tangent];
	RefHeading h;
	int angle = e.angle;
	if (!swapped) {
		h.x = e.x;
		h.y = e.y;
	} else {
		h.y = e.x;
		h.x = e.y;
		angle = (-1 - angle) & 0xF;
	}
	if (!yLess) {
		h.y = -h.y;
		angle = (-1 - angle) & 0x1F;
	}
	if (xLess)
		angle = (-1 - angle) & 0x3F;
	else
		h.x = -h.x;
	h.dir = (angle + 8) & 0x3F;
	return h;
}

TEST_F(D2Harness, TangentTableMatchesD2CommonDll)
{
	size_t offset = 0;
	const std::vector<RefEntry> lut = ReadD2CommonTable(offset);
	if (lut.empty())
		GTEST_SKIP() << "D2_COMMON_DLL not set or table not found";
	std::printf("D2Common.dll tangent table at file offset 0x%zx\n", offset);
	for (int t = 0; t < 128; t++) {
		const d2::D2Heading ours = d2::D2TangentTableEntry(t);
		EXPECT_EQ(ours.x, lut[t].x) << "entry " << t;
		EXPECT_EQ(ours.y, lut[t].y) << "entry " << t;
		EXPECT_EQ(ours.dir64, lut[t].angle) << "entry " << t;
	}
}

TEST_F(D2Harness, HeadingMatchesD2ReferenceForEveryDelta)
{
	size_t offset = 0;
	const std::vector<RefEntry> lut = ReadD2CommonTable(offset);
	if (lut.empty())
		GTEST_SKIP() << "D2_COMMON_DLL not set or table not found";
	int checked = 0;
	for (int dx = -300; dx <= 300; dx += 3) {
		for (int dy = -300; dy <= 300; dy += 3) {
			const d2::D2Heading ours = d2::D2DirectionVector(dx, dy);
			const RefHeading ref = RefDirection(lut, 0, 0, dx, dy);
			ASSERT_EQ(ours.x, ref.x) << dx << "," << dy;
			ASSERT_EQ(ours.y, ref.y) << dx << "," << dy;
			ASSERT_EQ(ours.dir64, ref.dir) << dx << "," << dy;
			checked++;
		}
	}
	std::printf("%d headings identical to the D2 reference\n", checked);
}

TEST_F(D2Harness, FacingUsesD2DirectionsOnD1Sprites)
{
	// The 8 tile steps must face the same way Diablo 1 faces for them
	const struct {
		Displacement d;
		Direction dir;
	} cases[] = {
		{ { 1, 1 }, Direction::South }, { { 0, 1 }, Direction::SouthWest }, { { -1, 1 }, Direction::West }, { { -1, 0 }, Direction::NorthWest },
		{ { -1, -1 }, Direction::North }, { { 0, -1 }, Direction::NorthEast }, { { 1, -1 }, Direction::East }, { { 1, 0 }, Direction::SouthEast },
	};
	for (const auto &c : cases) {
		const d2::D2Heading heading = d2::D2DirectionVector(c.d.deltaX * 256, c.d.deltaY * 256);
		EXPECT_EQ(d2::FacingFromDir64(heading.dir64), c.dir) << c.d.deltaX << "," << c.d.deltaY << " dir64 " << heading.dir64;
	}
}

struct SpeedCase {
	int dx, dy;
};
constexpr SpeedCase SpeedCases[] = {
	{ 20, 0 }, { 0, 20 }, { -20, 0 }, { 0, -20 }, { 14, 14 }, { -14, 14 }, { 14, -14 }, { -14, -14 },
	{ 20, 7 }, { 7, 20 }, { -20, 9 }, { 13, -17 }, { 20, -3 }, { -5, -19 }, { 18, 11 }, { -11, 18 },
};

/**
 * Ticks stock Diablo 1 takes to walk from Start to Start + (dx, dy) (or jog, in town with Run in Town),
 * measured by running the stock code path in this same harness.
 */
int StockTicks(int dx, int dy, bool town)
{
	sgOptions.Gameplay.d2Movement.SetValue(false);
	ClearLevel();
	leveltype = town ? DTYPE_TOWN : DTYPE_CATHEDRAL;
	sgGameInitInfo.bRunInTown = town ? 1 : 0;
	Player &player = SetupHero(Start, false);
	const Point target = Start + Displacement { dx, dy };
	ClrPlrPath(player);
	MakePlrPath(player, target, true);
	int ticks = 0;
	for (; ticks < 600 && (player.walkpath[0] != WALK_NONE || player._pmode != PM_STAND); ticks++)
		ProcessPlayers();
	EXPECT_EQ(player.position.tile, target) << "stock walk " << dx << "," << dy;
	sgOptions.Gameplay.d2Movement.SetValue(true);
	sgGameInitInfo.bRunInTown = 0;
	return ticks;
}

/** Ticks the mod takes for the same trip (hero running wherever running is allowed). */
int ModTicks(int dx, int dy, bool town)
{
	ClearLevel();
	leveltype = town ? DTYPE_TOWN : DTYPE_CATHEDRAL;
	Player &player = SetupHero(Start, true);
	const Point target = Start + Displacement { dx, dy };
	d2::FreeMoveSetTarget(player, target, 0, 0, true);
	int ticks = 0;
	for (; ticks < 600 && d2::FreeMoveActive(player); ticks++) {
		ProcessPlayers();
		d2probe::CheckTick();
	}
	EXPECT_EQ(player.position.tile, target) << "mod walk " << dx << "," << dy;
	EXPECT_EQ(player._pmode, PM_STAND);
	EXPECT_FALSE(player.freeMove.animating) << "hero kept the walk animation after stopping";
	return ticks;
}

TEST_F(D2Harness, PacingMatchesStockDiablo1)
{
	// Dungeon: never faster than a stock step even with running toggled on. Town: the stock Run in Town jog.
	// Stock walking also spends a fixed couple of ticks starting and stopping each order (free movement starts
	// on the same tick), so trips are compared per step: measure that fixed part first.
	int worst = 0;
	for (int town = 0; town < 2; town++) {
		const int ticks10 = StockTicks(10, 0, town != 0);
		const int ticks20 = StockTicks(20, 0, town != 0);
		const int pace = (ticks20 - ticks10) / 10;
		const int overhead = ticks10 - 10 * pace;
		std::printf("PACING %s stock pace %d ticks a step, %d ticks to start and stop\n", town != 0 ? "town-run" : "dungeon ", pace, overhead);
		for (const SpeedCase &c : SpeedCases) {
			const int stock = StockTicks(c.dx, c.dy, town != 0) - overhead;
			const int mod = ModTicks(c.dx, c.dy, town != 0);
			worst = std::max(worst, std::abs(mod - stock));
			std::printf("PACING %s d=(%d,%d) stock=%d ticks mod=%d ticks\n", town != 0 ? "town-run" : "dungeon ", c.dx, c.dy, stock, mod);
			EXPECT_LE(std::abs(mod - stock), 1) << (town != 0 ? "town" : "dungeon") << " trip " << c.dx << "," << c.dy << " differs from stock Diablo 1";
		}
	}
	std::printf("PACING worst=%d ticks\n", worst);
}

TEST_F(D2Harness, PathStaysOnTheStraightLine)
{
	for (const SpeedCase &c : SpeedCases) {
		ClearLevel();
		Player &player = SetupHero(Start, false);
		const Point target = Start + Displacement { c.dx, c.dy };
		d2::FreeMoveSetTarget(player, target, 0, 0, true);
		double worst = 0;
		const double len = std::hypot(c.dx, c.dy) * 256;
		for (int tick = 0; tick < 600 && d2::FreeMoveActive(player); tick++) {
			ProcessPlayers();
			const double px = player.freeMove.x - Start.x * 256.0, py = player.freeMove.y - Start.y * 256.0;
			worst = std::max(worst, std::abs(c.dx * 256.0 * py - c.dy * 256.0 * px) / len);
		}
		// Diablo 2's table quantises the heading to 1/127; re-aiming every step keeps the drift tiny
		std::printf("LINE d=(%d,%d) worst %.2f sub-tile units\n", c.dx, c.dy, worst);
		EXPECT_LE(worst, 16.0) << c.dx << "," << c.dy << " strays " << worst << " sub-tile units";
	}
}

TEST_F(D2Harness, WalkCycleAndFootstepsFollowTheStride)
{
	for (int town = 0; town < 2; town++) {
		ClearLevel();
		leveltype = town != 0 ? DTYPE_TOWN : DTYPE_CATHEDRAL;
		// Running is toggled on in both: the dungeon still walks (with footsteps), the town jogs (without)
		Player &player = SetupHero(Start, true);
		const uint64_t stepsBefore = d2probe::Count(d2probe::Id::sound_PlaySfxLoc);
		d2::FreeMoveSetTarget(player, Start + Displacement { 16, 0 }, 0, 0, true);
		int frames = 0;
		int ticks = 0;
		int8_t last = player.AnimInfo.currentFrame;
		for (; ticks < 600 && d2::FreeMoveActive(player); ticks++) {
			ProcessPlayers();
			const int8_t now = player.AnimInfo.currentFrame;
			frames += (now - last + 8) % 8;
			last = now;
		}
		const uint64_t steps = d2probe::Count(d2probe::Id::sound_PlaySfxLoc) - stepsBefore;
		// Diablo 1's walk cycle covers one tile per 8 frames; 16 tiles should show about 128 frames
		std::printf("STRIDE %s frames=%d (expect ~128) footsteps=%llu ticks=%d\n", town != 0 ? "run" : "walk", frames, static_cast<unsigned long long>(steps), ticks);
		EXPECT_NEAR(frames, 128, 10);
		if (town != 0) {
			EXPECT_EQ(steps, 0U) << "Diablo 1 plays no footsteps while jogging";
		} else {
			// Frames 0 and 4 of each cycle: two per tile
			EXPECT_NEAR(static_cast<double>(steps), 32.0, 3.0);
		}
	}
}

TEST_F(D2Harness, GamepadWalkKeepsAnimating)
{
	// The gamepad sends a walk order one tile ahead every tick. The stride must keep cycling, and the
	// "preview" first walk frame (which froze the cycle on screen) must not be shown while moving.
	ClearLevel();
	leveltype = DTYPE_CATHEDRAL;
	Player &player = SetupHero(Start, false);
	gbRunGame = true;
	gbProcessPlayers = true;
	PauseMode = 0;
	const uint64_t skippedBefore = d2probe::Count(d2probe::Id::d2_PreviewSkipped);
	int frames = 0;
	int8_t last = player.AnimInfo.currentFrame;
	for (int tick = 0; tick < 64; tick++) {
		const Point target = player.position.future + Direction::SouthEast;
		ClrPlrPath(player);
		MakePlrPath(player, target, true);
		player.destAction = ACTION_NONE;
		ProcessPlayers();
		d2probe::CheckTick();
		player.UpdatePreviewCelSprite(CMD_WALKXY, player.position.future + Direction::SouthEast, 0, 0);
		EXPECT_FALSE(static_cast<bool>(player.previewCelSprite)) << "walk preview shown mid-stride at tick " << tick;
		EXPECT_EQ(player.getGraphic(), player_graphic::Walk) << "not in the walk cycle at tick " << tick;
		const int8_t now = player.AnimInfo.currentFrame;
		frames += (now - last + 8) % 8;
		last = now;
	}
	gbRunGame = false;
	std::printf("GAMEPAD frames=%d over 64 ticks, previews skipped=%llu\n", frames, static_cast<unsigned long long>(d2probe::Count(d2probe::Id::d2_PreviewSkipped) - skippedBefore));
	EXPECT_NEAR(frames, 64, 2) << "walk cycle did not advance one frame per tick";
	EXPECT_GT(d2probe::Count(d2probe::Id::d2_PreviewSkipped), skippedBefore);
	EXPECT_EQ(player.position.tile, (Start + Displacement { 8, 0 })) << "64 ticks of walking should cover 8 tiles";
}

void SolidObject(int index, Point tile)
{
	Objects[index] = {};
	Objects[index]._oSolidFlag = true;
	Objects[index].position = tile;
	dObject[tile.x][tile.y] = static_cast<int8_t>(index + 1);
}

TEST_F(D2Harness, SlipsBetweenBarrelsAtAnAngle)
{
	// Two barrels with a one tile gap between them; approach from below at many angles and starting spots
	int trips = 0;
	int slides = 0;
	for (int sx = 40; sx <= 50; sx++) {
		for (int sy = 44; sy <= 47; sy++) {
			for (const Point target : { Point { 45, 36 }, Point { 42, 35 }, Point { 48, 35 } }) {
				for (int fine = 0; fine < 3; fine++) {
					ClearLevel();
					leveltype = DTYPE_CATHEDRAL;
					SolidObject(0, { 44, 40 });
					SolidObject(1, { 46, 40 });
					// a wall either side, so the gap is the only way through
					for (int x = 30; x <= 43; x++)
						Wall({ x, 40 });
					for (int x = 47; x <= 60; x++)
						Wall({ x, 40 });
					Player &player = SetupHero({ sx, sy }, false);
					const int fx[3] = { 0, 90, -100 };
					const int fy[3] = { 0, -70, 60 };
					const uint64_t slidesBefore = d2probe::Count(d2probe::Id::d2_MoveSlide);
					d2::FreeMoveSetTarget(player, target, fx[fine], fy[fine], true);
					int tick = 0;
					for (; tick < 600 && d2::FreeMoveActive(player); tick++) {
						ProcessPlayers();
						d2probe::CheckTick();
						ASSERT_FALSE(player.position.tile == Point(44, 40) || player.position.tile == Point(46, 40)) << "inside a barrel";
					}
					slides += static_cast<int>(d2probe::Count(d2probe::Id::d2_MoveSlide) - slidesBefore);
					EXPECT_EQ(player.position.tile, target) << "stuck from (" << sx << "," << sy << ") fine " << fine << " heading to (" << target.x << "," << target.y << ") at (" << player.position.tile.x << "," << player.position.tile.y << ")";
					trips++;
				}
			}
		}
	}
	std::printf("BARRELS trips=%d slides=%d\n", trips, slides);
}

TEST_F(D2Harness, SqueezesDiagonallyPastChests)
{
	// Two chests touching corners right on the way: Diablo 1 lets heroes step diagonally between them
	int trips = 0;
	int hops = 0;
	for (int sx = 40; sx <= 44; sx++) {
		for (int sy = 42; sy <= 46; sy++) {
			for (const Point target : { Point { 48, 37 }, Point { 46, 36 }, Point { 49, 39 } }) {
				for (int fine = 0; fine < 3; fine++) {
					ClearLevel();
					leveltype = DTYPE_CATHEDRAL;
					SolidObject(0, { 45, 41 });
					SolidObject(1, { 44, 40 });
					Player &player = SetupHero({ sx, sy }, false);
					if (IsTileSolid({ sx, sy }) || IsTileSolid(target))
						continue;
					const int fx[3] = { 0, 90, -100 };
					const int fy[3] = { 0, -70, 60 };
					const uint64_t hopsBefore = d2probe::Count(d2probe::Id::d2_CornerHop);
					d2::FreeMoveSetTarget(player, target, fx[fine], fy[fine], true);
					for (int tick = 0; tick < 600 && d2::FreeMoveActive(player); tick++) {
						ProcessPlayers();
						d2probe::CheckTick();
						ASSERT_FALSE(player.position.tile == Point(45, 41) || player.position.tile == Point(44, 40)) << "inside a chest";
					}
					hops += static_cast<int>(d2probe::Count(d2probe::Id::d2_CornerHop) - hopsBefore);
					// Only trips stock Diablo 1 path finding can make count
					int8_t path[MaxPathLength];
					if (FindPath([&player](Point position) { return PosOkPlayer(player, position); }, Point { sx, sy }, target, path) == 0)
						continue;
					EXPECT_EQ(player.position.tile, target) << "stuck from (" << sx << "," << sy << ") fine " << fine << " heading to (" << target.x << "," << target.y << ") at (" << player.position.tile.x << "," << player.position.tile.y << ")";
					trips++;
				}
			}
		}
	}
	std::printf("CHESTS trips=%d corner_cuts=%d\n", trips, hops);
	EXPECT_GT(trips, 0);
}

/** Screen directions as world steps: the 8 ways a stick or the arrow keys push. */
constexpr Displacement ScreenSteps[8] = { { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 } };

TEST_F(D2Harness, NeverStuckNextToAChest)
{
	// Press into a chest from every side and corner (as a stick or a held mouse does), then turn and push
	// the other way. Wherever a stock Diablo 1 step would be possible, the hero must get going again.
	const Point chest { 45, 45 };
	int cases = 0;
	int stuck = 0;
	for (int ox = -2; ox <= 2; ox++) {
		for (int oy = -2; oy <= 2; oy++) {
			const Point start = chest + Displacement { ox, oy };
			if (start == chest)
				continue;
			for (int into = 0; into < 8; into++) {
				for (int then = 0; then < 8; then++) {
					ClearLevel();
					leveltype = DTYPE_CATHEDRAL;
					SolidObject(0, chest);
					Player &player = SetupHero(start, false);
					// Phase 1: push towards the chest for a while (gamepad style: one tile ahead, every tick)
					for (int tick = 0; tick < 24; tick++) {
						const Point ahead = player.position.future + ScreenSteps[into];
						ClrPlrPath(player);
						MakePlrPath(player, ahead, true);
						player.destAction = ACTION_NONE;
						ProcessPlayers();
						d2probe::CheckTick();
					}
					// Phase 2: push another way; if a stock step that way is possible, the hero must leave the tile
					const Point from = player.position.tile;
					const Point next = from + ScreenSteps[then];
					const Point after = next + ScreenSteps[then];
					if (!PosOkPlayer(player, next) || !PosOkPlayer(player, after) || !path_solid_pieces(from, next))
						continue;
					cases++;
					bool left = false;
					for (int tick = 0; tick < 24 && !left; tick++) {
						const Point ahead = player.position.future + ScreenSteps[then];
						ClrPlrPath(player);
						MakePlrPath(player, ahead, true);
						player.destAction = ACTION_NONE;
						ProcessPlayers();
						d2probe::CheckTick();
						left = player.position.tile != from;
					}
					if (!left) {
						stuck++;
						if (stuck <= 10)
							ADD_FAILURE() << "stuck at (" << from.x - chest.x << "," << from.y - chest.y << ") from the chest, sub-tile ("
							              << player.freeMove.x - from.x * 256 << "," << player.freeMove.y - from.y * 256 << "), after pushing "
							              << into << " then " << then;
					}
				}
			}
		}
	}
	std::printf("CHESTSIDE cases=%d stuck=%d\n", cases, stuck);
}

enum class Blocker { Chest, Sarcophagus, Monster, Townsperson, OtherHero, Wall, PillarAndSkeleton };
enum class Control { MouseHeld, MouseClick, Gamepad };

const char *Name(Blocker b)
{
	switch (b) {
	case Blocker::Chest: return "chest";
	case Blocker::Sarcophagus: return "sarcophagus (two tiles)";
	case Blocker::PillarAndSkeleton: return "pillar with a skeleton beside it";
	case Blocker::Monster: return "monster";
	case Blocker::Townsperson: return "townsperson";
	case Blocker::OtherHero: return "other hero";
	default: return "wall";
	}
}

const char *Name(Control c)
{
	switch (c) {
	case Control::MouseHeld: return "mouse held";
	case Control::MouseClick: return "mouse click";
	default: return "gamepad";
	}
}

void PlaceBlocker(Blocker kind, Point tile)
{
	leveltype = kind == Blocker::Townsperson ? DTYPE_TOWN : DTYPE_CATHEDRAL;
	switch (kind) {
	case Blocker::Chest:
		SolidObject(0, tile);
		break;
	case Blocker::Sarcophagus:
		// A large object: its main tile, plus the tile behind it marked as part of it
		SolidObject(0, tile);
		dObject[tile.x][tile.y - 1] = -1;
		break;
	case Blocker::PillarAndSkeleton:
		Wall(tile);
		Monsters[0] = {};
		Monsters[0].hitPoints = 100 << 6;
		Monsters[0].position.tile = Monsters[0].position.future = Monsters[0].position.old = tile + Displacement { 2, 0 };
		dMonster[tile.x + 2][tile.y] = 1;
		break;
	case Blocker::Monster:
	case Blocker::Townsperson:
		Monsters[0] = {};
		Monsters[0].hitPoints = 100 << 6;
		Monsters[0].position.tile = Monsters[0].position.future = Monsters[0].position.old = tile;
		dMonster[tile.x][tile.y] = 1;
		break;
	case Blocker::OtherHero:
		Players[1] = {};
		Players[1]._pHitPoints = 100 << 6;
		dPlayer[tile.x][tile.y] = 2;
		break;
	case Blocker::Wall:
		Wall(tile);
		break;
	}
}

/** What the game's mouse code (SendWalkToCursor) sends while the button is held over (x, y). */
struct MouseState {
	int32_t lastX = INT32_MIN;
	int32_t lastY = INT32_MIN;
};

void SendFine(Player &player, int32_t x, int32_t y)
{
	const Point tile { (x + 128) >> 8, (y + 128) >> 8 };
	const int fx = x - tile.x * 256 + 128;
	const int fy = y - tile.y * 256 + 128;
	d2::OnWalkFine(player, tile, static_cast<uint16_t>(fx | (fy << 8)));
}

void MouseHeldTick(Player &player, MouseState &mouse, int32_t x, int32_t y)
{
	const d2::FreeMoveState &move = player.freeMove;
	if (std::abs(x - mouse.lastX) < 32 && std::abs(y - mouse.lastY) < 32) {
		const bool arrived = std::abs(move.x - mouse.lastX) < 32 && std::abs(move.y - mouse.lastY) < 32;
		if (move.active || arrived)
			return;
	}
	mouse.lastX = x;
	mouse.lastY = y;
	SendFine(player, x, y);
}

void GamepadTick(Player &player, Displacement dir)
{
	const Point ahead = player.position.future + dir;
	ClrPlrPath(player);
	MakePlrPath(player, ahead, true);
	player.destAction = ACTION_NONE;
}

TEST_F(D2Harness, NothingTrapsTheHero)
{
	// Your exact situations, for everything that can stand in the way: walk into a chest, monster, townsperson,
	// another hero or a wall (mouse held, mouse click or gamepad), keep pushing, then head off somewhere else.
	// Wherever stock Diablo 1 can walk, the hero must get there.
	const Point blocker { 45, 45 };
	int cases = 0;
	int failures = 0;
	for (const Blocker kind : { Blocker::Chest, Blocker::Sarcophagus, Blocker::Monster, Blocker::Townsperson, Blocker::OtherHero, Blocker::Wall, Blocker::PillarAndSkeleton }) {
		for (const Control control : { Control::MouseHeld, Control::MouseClick, Control::Gamepad }) {
			int kindFailures = 0;
			for (int side = 0; side < 8; side++) {
				for (int fine = 0; fine < 3; fine++) {
					for (int away = 0; away < 16; away++) {
						ClearLevel();
						PlaceBlocker(kind, blocker);
						const Displacement towards = ScreenSteps[(side + 4) % 8];
						const Point start = blocker + ScreenSteps[side] + ScreenSteps[side];
						if (dMonster[start.x][start.y] != 0 || dObject[start.x][start.y] != 0 || IsTileSolid(start))
							continue;
						Player &player = SetupHero(start, false);
						player.isRunning = true;
						MouseState mouse;
						const int fo[3] = { 0, 70, -90 };
						// Phase 1: walk into the blocker and keep pushing
						const int32_t intoX = (blocker.x + 2 * towards.deltaX) * 256 + fo[fine];
						const int32_t intoY = (blocker.y + 2 * towards.deltaY) * 256 - fo[fine];
						for (int tick = 0; tick < 40; tick++) {
							if (control == Control::MouseHeld)
								MouseHeldTick(player, mouse, intoX, intoY);
							else if (control == Control::Gamepad)
								GamepadTick(player, towards);
							else if (tick == 0)
								SendFine(player, intoX, intoY);
							ProcessPlayers();
							d2probe::CheckTick();
						}
						// Phase 2: head somewhere else, 4 tiles out from the blocker
						const double a = away * 3.14159265358979 / 8;
						const Point goal = blocker + Displacement { static_cast<int>(std::lround(std::cos(a) * 4)), static_cast<int>(std::lround(std::sin(a) * 4)) };
						if (goal == player.position.tile || !PosOkPlayer(player, goal))
							continue;
						int8_t path[MaxPathLength];
						if (FindPath([&player](Point position) { return PosOkPlayer(player, position); }, player.position.tile, goal, path) == 0)
							continue;
						cases++;
						const int32_t gx = goal.x * 256 + fo[(fine + 1) % 3], gy = goal.y * 256 + fo[(fine + 2) % 3];
						bool arrived = false;
						for (int tick = 0; tick < 300 && !arrived; tick++) {
							if (control == Control::MouseHeld) {
								MouseHeldTick(player, mouse, gx, gy);
							} else if (control == Control::Gamepad) {
								// steer the stick along the stock path, one step at a time, as a player would
								int8_t steps[MaxPathLength];
								if (FindPath([&player](Point position) { return PosOkPlayer(player, position); }, player.position.tile, goal, steps) > 0)
									GamepadTick(player, d2::WalkStepDisplacement(steps[0]));
							} else if (tick == 0) {
								SendFine(player, gx, gy);
							}
							ProcessPlayers();
							d2probe::CheckTick();
							arrived = player.position.tile == goal && !d2::FreeMoveActive(player) && player._pmode == PM_STAND;
						}
						if (!arrived) {
							failures++;
							if (++kindFailures <= 2)
								ADD_FAILURE() << Name(kind) << ", " << Name(control) << ": walked in from side " << side << ", then stuck at ("
								              << player.position.tile.x - blocker.x << "," << player.position.tile.y - blocker.y << ") heading for ("
								              << goal.x - blocker.x << "," << goal.y - blocker.y << ")";
						}
					}
				}
			}
			std::printf("TRAP %s / %s: %d stuck\n", Name(kind), Name(control), kindFailures);
		}
	}
	std::printf("TRAPS cases=%d stuck=%d\n", cases, failures);
	EXPECT_GT(cases, 3000);
}

TEST_F(D2Harness, SafetyNetHandsOverToStockWalking)
{
	// Force free movement into a dead end (a pocket, with its route deliberately pointed through a wall and its
	// re-routing used up). The safety net must notice within a few ticks and let stock walking take the hero home.
	ClearLevel();
	leveltype = DTYPE_CATHEDRAL;
	for (int i = -1; i <= 1; i++) {
		Wall(Start + Displacement { i, -1 });
		Wall(Start + Displacement { 1, i });
		Wall(Start + Displacement { i, 1 });
	}
	Player &player = SetupHero(Start, false);
	const Point goal = Start + Displacement { 6, 0 };
	d2::FreeMoveSetTarget(player, goal, 0, 0, true);
	ASSERT_TRUE(d2::FreeMoveActive(player));
	// Sabotage: straight through the wall, no re-routing left
	player.freeMove.waypointCount = 1;
	player.freeMove.waypointIndex = 0;
	player.freeMove.waypointX[0] = goal.x * 256;
	player.freeMove.waypointY[0] = goal.y * 256;
	player.freeMove.repaths = 3;
	const uint64_t fallbacksBefore = d2probe::Count(d2probe::Id::d2_StuckFallback);
	bool sawStockWalk = false;
	int tick = 0;
	for (; tick < 300 && player.position.tile != goal; tick++) {
		ProcessPlayers();
		d2probe::CheckTick();
		sawStockWalk = sawStockWalk || player.isWalking();
		// a held mouse keeps re-sending the same destination: it must not cancel the stock walk
		if (player.freeMove.handover)
			d2::OnWalkFine(player, goal, 0x8080);
	}
	std::printf("SAFETYNET fallbacks=%llu stockwalk=%d ticks=%d\n", static_cast<unsigned long long>(d2probe::Count(d2probe::Id::d2_StuckFallback) - fallbacksBefore), sawStockWalk ? 1 : 0, tick);
	EXPECT_GT(d2probe::Count(d2probe::Id::d2_StuckFallback), fallbacksBefore) << "safety net never engaged";
	EXPECT_TRUE(sawStockWalk) << "stock walking never took over";
	EXPECT_EQ(player.position.tile, goal) << "did not get home";
	for (int t = 0; t < 20; t++)
		ProcessPlayers();
	EXPECT_EQ(player._pmode, PM_STAND);
	EXPECT_FALSE(player.freeMove.handover) << "safety net did not let go after arriving";
	EXPECT_FALSE(player.freeMove.wantGoal);
}

TEST_F(D2Harness, NeverStuckAmongScatteredObjects)
{
	// A room strewn with chests and barrels: every trip stock path finding can make, the mod must finish
	uint32_t seed = 12345;
	auto random = [&seed](int n) {
		seed = seed * 1103515245U + 12345U;
		return static_cast<int>((seed >> 16) % static_cast<uint32_t>(n));
	};
	int trips = 0;
	int stuck = 0;
	for (int layout = 0; layout < 40; layout++) {
		ClearLevel();
		leveltype = DTYPE_CATHEDRAL;
		int objects = 0;
		for (int i = 0; i < 70; i++) {
			const Point tile { 30 + random(24), 30 + random(24) };
			if (dObject[tile.x][tile.y] == 0 && objects < 120)
				SolidObject(objects++, tile);
		}
		for (int t = 0; t < 12; t++) {
			const Point start { 30 + random(24), 30 + random(24) };
			const Point target { 30 + random(24), 30 + random(24) };
			if (dObject[start.x][start.y] != 0 || dObject[target.x][target.y] != 0 || start == target)
				continue;
			std::memset(dPlayer, 0, sizeof(dPlayer));
			Player &player = SetupHero(start, false);
			int8_t path[MaxPathLength];
			if (FindPath([&player](Point position) { return PosOkPlayer(player, position); }, start, target, path) == 0)
				continue;
			d2::FreeMoveSetTarget(player, target, random(200) - 100, random(200) - 100, true);
			for (int tick = 0; tick < 900 && d2::FreeMoveActive(player); tick++) {
				ProcessPlayers();
				d2probe::CheckTick();
				ASSERT_EQ(dObject[player.position.tile.x][player.position.tile.y], 0) << "inside an object";
			}
			trips++;
			if (player.position.tile != target) {
				stuck++;
				ADD_FAILURE() << "layout " << layout << ": stuck at (" << player.position.tile.x << "," << player.position.tile.y << ") going from ("
				              << start.x << "," << start.y << ") to (" << target.x << "," << target.y << ")";
			}
		}
	}
	std::printf("OBJECTS trips=%d stuck=%d corner_cuts=%llu slides=%llu repaths=%llu\n", trips, stuck,
	    static_cast<unsigned long long>(d2probe::Count(d2probe::Id::d2_CornerHop)),
	    static_cast<unsigned long long>(d2probe::Count(d2probe::Id::d2_MoveSlide)),
	    static_cast<unsigned long long>(d2probe::Count(d2probe::Id::d2_Repath)));
	EXPECT_GT(trips, 200);
}

TEST_F(D2Harness, FacesTheWayItMovesOnScreen)
{
	// Walk straight in 72 directions: the sprite must be the walk sprite closest to the motion on screen
	// (independently computed here from the screen angle) and must not flicker during the walk
	const double spriteAngles[8] = { 90.0, 153.43, 180.0, 206.57, 270.0, 333.43, 0.0, 26.57 }; // S SW W NW N NE E SE, screen degrees (y down)
	int wrong = 0;
	int flickers = 0;
	for (int a = 0; a < 72; a++) {
		const double angle = a * 5.0 + 2.5; // avoid exact sprite boundaries
		const double rad = angle * 3.14159265358979 / 180.0;
		// screen direction -> world delta: sx = (dx - dy) * 32, sy = (dx + dy) * 16
		const double sxv = std::cos(rad), syv = std::sin(rad);
		const double wdx = (sxv / 32.0 + syv / 16.0) / 2.0, wdy = (syv / 16.0 - sxv / 32.0) / 2.0;
		const double scale = 14.0 / std::max(std::abs(wdx), std::abs(wdy));
		const Point target = Start + Displacement { static_cast<int>(std::lround(wdx * scale)), static_cast<int>(std::lround(wdy * scale)) };
		ClearLevel();
		leveltype = DTYPE_CATHEDRAL;
		Player &player = SetupHero(Start, false);
		d2::FreeMoveSetTarget(player, target, 0, 0, true);
		// expected sprite from the real screen angle of this trip
		const int tdx = target.x - Start.x, tdy = target.y - Start.y;
		const double realAngle = std::atan2((tdx + tdy) * 16.0, (tdx - tdy) * 32.0) * 180.0 / 3.14159265358979;
		int expected = 0;
		double bestDiff = 1e9;
		for (int d = 0; d < 8; d++) {
			double diff = std::fmod(std::abs(realAngle - spriteAngles[d]) + 360.0, 360.0);
			diff = std::min(diff, 360.0 - diff);
			if (diff < bestDiff) {
				bestDiff = diff;
				expected = d;
			}
		}
		Direction last = player._pdir;
		int changes = 0;
		for (int tick = 0; tick < 600 && d2::FreeMoveActive(player); tick++) {
			ProcessPlayers();
			if (d2::FreeMoveActive(player) && tick > 0 && player.freeMove.facing != last)
				changes++;
			last = player.freeMove.facing;
		}
		if (static_cast<int>(last) != expected) {
			// within 4 degrees of a boundary either neighbour is acceptable (hysteresis)
			double diff = std::fmod(std::abs(realAngle - spriteAngles[static_cast<int>(last)]) + 360.0, 360.0);
			diff = std::min(diff, 360.0 - diff);
			if (diff > bestDiff + 4.0) {
				wrong++;
				ADD_FAILURE() << "moving at " << realAngle << " degrees on screen faces sprite " << static_cast<int>(last) << ", expected " << expected;
			}
		}
		if (changes > 0) {
			flickers++;
			ADD_FAILURE() << "facing changed " << changes << " times walking straight at " << realAngle << " degrees";
		}
	}
	std::printf("FACING directions=72 wrong=%d flickering=%d\n", wrong, flickers);
}

/** Angle on screen (degrees, y down) of a world delta, and how far a sprite direction is from it. */
double ScreenAngle(double dx, double dy)
{
	return std::atan2((dx + dy) * 16.0, (dx - dy) * 32.0) * 180.0 / 3.14159265358979;
}

double SpriteOffBy(Direction dir, double angle)
{
	constexpr double SpriteAngles[8] = { 90.0, 153.43, 180.0, 206.57, 270.0, 333.43, 0.0, 26.57 };
	double diff = std::fmod(std::abs(angle - SpriteAngles[static_cast<int>(dir)]) + 360.0, 360.0);
	return std::min(diff, 360.0 - diff);
}

TEST_F(D2Harness, FacingFollowsTheMouse)
{
	// Hold the mouse and sweep it around the hero, and click around a room of objects: on every tick the hero
	// moves, the sprite must be the one closest to how the hero is actually moving on screen (judged over
	// the last 3 ticks), give or take the few degrees of hysteresis that stop flicker
	uint32_t seed = 777;
	auto random = [&seed](int n) {
		seed = seed * 1103515245U + 12345U;
		return static_cast<int>((seed >> 16) % static_cast<uint32_t>(n));
	};
	int checked = 0;
	int wrong = 0;
	double worst = 0;
	for (int scenario = 0; scenario < 2; scenario++) {
		ClearLevel();
		leveltype = DTYPE_CATHEDRAL;
		if (scenario == 1) {
			for (int i = 0; i < 40; i++) {
				const Point tile { 30 + random(24), 30 + random(24) };
				if (tile != Start && dObject[tile.x][tile.y] == 0)
					SolidObject(i, tile);
			}
		}
		Player &player = SetupHero(Start, false);
		int32_t histX[4] = {}, histY[4] = {};
		bool lastTickMismatched = false;
		for (int tick = 0; tick < 3000; tick++) {
			if (scenario == 0) {
				// cursor circling the hero at 3 tiles, a little further round every tick
				const double a = tick * 0.02;
				const int32_t cx = player.freeMove.x + static_cast<int32_t>(std::cos(a) * 768);
				const int32_t cy = player.freeMove.y + static_cast<int32_t>(std::sin(a) * 768);
				const Point tile { (cx + 128) >> 8, (cy + 128) >> 8 };
				d2::OnWalkFine(player, tile, static_cast<uint16_t>(((cx - tile.x * 256 + 128) & 0xFF) | (((cy - tile.y * 256 + 128) & 0xFF) << 8)));
			} else if (!d2::FreeMoveActive(player) || random(40) == 0) {
				const Point target { 30 + random(24), 30 + random(24) };
				if (dObject[target.x][target.y] == 0)
					d2::OnWalkFine(player, target, static_cast<uint16_t>(random(256) | (random(256) << 8)));
			}
			const int32_t beforeX = player.freeMove.x, beforeY = player.freeMove.y;
			ProcessPlayers();
			d2probe::CheckTick();
			for (int k = 3; k > 0; k--) {
				histX[k] = histX[k - 1];
				histY[k] = histY[k - 1];
			}
			histX[0] = player.freeMove.x - beforeX;
			histY[0] = player.freeMove.y - beforeY;
			const int32_t mx = histX[0] + histX[1] + histX[2], my = histY[0] + histY[1] + histY[2];
			if (!d2::FreeMoveActive(player) || histX[0] == 0 && histY[0] == 0 || std::max(std::abs(mx), std::abs(my)) < 48) {
				lastTickMismatched = false;
				continue;
			}
			// Judge steady motion only: right after a turn the last few ticks still point the old way
			bool steady = true;
			for (int k = 1; k < 3; k++) {
				double turn = std::fmod(std::abs(ScreenAngle(histX[k], histY[k]) - ScreenAngle(histX[0], histY[0])) + 360.0, 360.0);
				steady = steady && std::min(turn, 360.0 - turn) < 8.0;
			}
			if (!steady) {
				lastTickMismatched = false;
				continue;
			}
			const double angle = ScreenAngle(mx, my);
			double best = 1e9;
			for (int d = 0; d < 8; d++)
				best = std::min(best, SpriteOffBy(static_cast<Direction>(d), angle));
			const double off = SpriteOffBy(player.freeMove.facing, angle) - best;
			checked++;
			// The sprite turns as the hero reaches a path point, so on that one tick it already shows the next leg:
			// only a mismatch lasting two ticks in a row is a real one
			const bool mismatch = off > 8.0;
			const bool persistent = mismatch && lastTickMismatched;
			lastTickMismatched = mismatch;
			if (persistent)
				worst = std::max(worst, off);
			if (persistent) {
				wrong++;
				if (wrong <= 8)
					ADD_FAILURE() << "scenario " << scenario << " tick " << tick << ": moving at " << angle << " degrees on screen, sprite "
					              << static_cast<int>(player.freeMove.facing) << " is " << off << " degrees worse than the best; steps ("
					              << histX[2] << "," << histY[2] << ") (" << histX[1] << "," << histY[1] << ") (" << histX[0] << "," << histY[0] << ") wp "
					              << static_cast<int>(player.freeMove.waypointIndex) << "/" << static_cast<int>(player.freeMove.waypointCount) << " to ("
					              << player.freeMove.waypointX[player.freeMove.waypointIndex] - player.freeMove.x << "," << player.freeMove.waypointY[player.freeMove.waypointIndex] - player.freeMove.y << ")";
			}
		}
	}
	std::printf("MOUSEFACING ticks=%d wrong=%d worst=%.1f\n", checked, wrong, worst);
	EXPECT_GT(checked, 1000);
}

TEST_F(D2Harness, ArrowsLeaveTheBowAndLandOnTheTarget)
{
	// A hero standing off their tile centre shoots: the arrow must be drawn from where the hero really is and
	// end on the monster; an arrow shot at that hero must end where the hero really is. Drawing only.
	ClearLevel();
	leveltype = DTYPE_CATHEDRAL;
	Player &player = SetupHero(Start, false);
	player.freeMove.valid = true;
	player.freeMove.x = Start.x * 256 + 100;
	player.freeMove.y = Start.y * 256 - 60;
	const Displacement heroOffset = d2::GlideCorrection(player);
	ASSERT_NE(heroOffset, Displacement {});
	const Point monsterTile = Start + Displacement { 5, 2 };

	Missile arrow {};
	arrow._micaster = TARGET_MONSTERS;
	arrow._misource = 0;
	arrow.position.velocity = { 24 << 16, 8 << 16 };
	d2::AnchorMissileVisuals(arrow, Start, monsterTile);
	ASSERT_GT(arrow.d2FlightTicks, 0);
	EXPECT_EQ(d2::MissileVisualOffset(arrow, 0), heroOffset) << "arrow not drawn at the bow";
	arrow.d2Age = arrow.d2FlightTicks;
	EXPECT_EQ(d2::MissileVisualOffset(arrow, 0), Displacement {}) << "arrow not drawn landing on the monster";

	Missile incoming {};
	incoming._micaster = TARGET_PLAYERS;
	incoming._misource = 0;
	incoming.position.velocity = { -24 << 16, -8 << 16 };
	d2::AnchorMissileVisuals(incoming, monsterTile, Start);
	ASSERT_GT(incoming.d2FlightTicks, 0);
	EXPECT_EQ(d2::MissileVisualOffset(incoming, 0), Displacement {}) << "incoming arrow not drawn from the monster";
	incoming.d2Age = incoming.d2FlightTicks;
	EXPECT_EQ(d2::MissileVisualOffset(incoming, 0), heroOffset) << "incoming arrow not drawn landing on the hero";
	std::printf("ARROWS hero offset (%d,%d) px, flight %d ticks\n", heroOffset.deltaX, heroOffset.deltaY, arrow.d2FlightTicks);

	sgOptions.Gameplay.d2Movement.SetValue(false);
	Missile stock {};
	stock._micaster = TARGET_MONSTERS;
	stock.position.velocity = { 24 << 16, 8 << 16 };
	d2::AnchorMissileVisuals(stock, Start, monsterTile);
	EXPECT_EQ(d2::MissileVisualOffset(stock, 0), Displacement {}) << "stock mode must draw missiles exactly as before";
	sgOptions.Gameplay.d2Movement.SetValue(true);
}

TEST_F(D2Harness, GamepadGoesThroughDoorways)
{
	// An open door in a wall that runs across the screen diagonally. Push the stick straight up, or up-right
	// (straight at the doorway), from spots in front of it: the hero must end up through the door.
	int cases = 0;
	int failed = 0;
	for (const Direction push : { Direction::North, Direction::NorthEast, Direction::East }) {
		for (int sx = 44; sx <= 46; sx++) {
			for (int sy = 41; sy <= 43; sy++) {
				ClearLevel();
				leveltype = DTYPE_CATHEDRAL;
				for (int x = 30; x <= 60; x++)
					if (x != 45)
						Wall({ x, 40 });
				Objects[0] = {};
				Objects[0]._otype = OBJ_L1LDOOR;
				Objects[0]._oSolidFlag = false; // open
				Objects[0].position = { 45, 40 };
				dObject[45][40] = 1;
				Player &player = SetupHero({ sx, sy }, false);
				// steer towards the doorway column first, as a player lines up with a door
				int tick = 0;
				for (; tick < 120 && player.position.tile.y >= 40; tick++) {
					Point step;
					const Direction dir = player.position.tile.x < 45 ? Direction::East : (player.position.tile.x > 45 ? Direction::North : push);
					if (d2::GamepadStep(player, dir, step)) {
						ClrPlrPath(player);
						MakePlrPath(player, step, true);
						player.destAction = ACTION_NONE;
					}
					ProcessPlayers();
					d2probe::CheckTick();
				}
				cases++;
				if (player.position.tile.y >= 40) {
					failed++;
					ADD_FAILURE() << "pushing " << static_cast<int>(push) << " from (" << sx << "," << sy << ") never got through the door; at ("
					              << player.position.tile.x << "," << player.position.tile.y << ")";
				}
			}
		}
	}
	std::printf("DOORS cases=%d stuck=%d\n", cases, failed);
}

/** Where the renderer draws a tile delta (+x: right and down, +y: left and down). */
Displacement ScreenOf(int dx, int dy)
{
	return { (dx - dy) * 32, (dx + dy) * 16 };
}

TEST_F(D2Harness, HitMonstersEaseBackInsteadOfJumping)
{
	// A monster hit half-way through a step goes back to the tile it was leaving (stock rule, unchanged). It must
	// be drawn starting from where it was and easing back over a few ticks, not jumping.
	Monster monster {};
	monster.position.old = { 40, 40 };
	monster.position.tile = monster.position.future = { 41, 40 }; // stepping south-east: already counted on the new tile
	monster.direction = Direction::SouthEast;
	monster.mode = MonsterMode::MoveSouthwards;
	monster.animInfo.setNewAnimation(std::nullopt, 8, 1);
	for (int i = 0; i < 4; i++)
		monster.animInfo.processAnimation();
	const Displacement drawnBefore = ScreenOf(monster.position.tile.x - monster.position.old.x, monster.position.tile.y - monster.position.old.y)
	    + GetOffsetForWalking(monster.animInfo, monster.direction);
	d2::BeginMonsterHitSlide(monster);
	// the stock snap back
	monster.position.tile = monster.position.future = monster.position.old;
	monster.mode = MonsterMode::HitRecovery;
	ASSERT_NE(drawnBefore, Displacement {});
	EXPECT_EQ(d2::MonsterHitSlideOffset(monster, 0), drawnBefore) << "monster jumped on the hit frame";
	int moved = 0;
	Displacement last = drawnBefore;
	for (int tick = 0; tick < d2::MonsterHitSlideTicks; tick++) {
		monster.d2HitSlideTicks--;
		const Displacement now = d2::MonsterHitSlideOffset(monster, 0);
		moved += std::abs(now.deltaX - last.deltaX) + std::abs(now.deltaY - last.deltaY);
		last = now;
	}
	EXPECT_EQ(last, Displacement {}) << "monster did not finish easing back";
	std::printf("HITSLIDE from (%d,%d) px back to its tile over %d ticks\n", drawnBefore.deltaX, drawnBefore.deltaY, static_cast<int>(d2::MonsterHitSlideTicks));

	sgOptions.Gameplay.d2Movement.SetValue(false);
	monster.position.tile = monster.position.future = { 41, 40 };
	monster.mode = MonsterMode::MoveSouthwards;
	d2::BeginMonsterHitSlide(monster);
	EXPECT_EQ(d2::MonsterHitSlideOffset(monster, 0), Displacement {}) << "stock mode must draw monsters exactly as before";
	sgOptions.Gameplay.d2Movement.SetValue(true);
}

TEST_F(D2Harness, MovementReportHotkeyWritesTheLastSeconds)
{
	ClearLevel();
	leveltype = DTYPE_CATHEDRAL;
	Player &player = SetupHero(Start, false);
	d2::FreeMoveSetTarget(player, Start + Displacement { 5, 2 }, 0, 0, true);
	for (int tick = 0; tick < 40; tick++) {
		ProcessPlayers();
		d2::RecordTrace();
	}
	const std::string dir = paths::PrefPath();
	std::vector<std::string> before;
	for (const auto &entry : std::filesystem::directory_iterator(dir))
		before.push_back(entry.path().filename().string());
	d2::WriteMovementReport();
	std::string report;
	for (const auto &entry : std::filesystem::directory_iterator(dir)) {
		const std::string name = entry.path().filename().string();
		if (name.rfind("d2movement-report-", 0) == 0 && std::find(before.begin(), before.end(), name) == before.end())
			report = entry.path().string();
	}
	ASSERT_FALSE(report.empty()) << "no report file written in " << dir;
	std::ifstream in(report);
	int lines = 0;
	std::string line;
	while (std::getline(in, line))
		lines++;
	std::printf("REPORT %d lines\n", lines);
	EXPECT_GT(lines, 40);
}

TEST_F(D2Harness, HitMonstersStopOnTheNearerTile)
{
	// Every frame of a step south-east: a hit before halfway settles back on the tile it was leaving, a hit after
	// halfway finishes the step. Either way the monster never appears to move more than half a tile.
	int finished = 0;
	int returned = 0;
	int worstJump = 0;
	for (int frame = 0; frame < 8; frame++) {
		Monster monster {};
		monster.position.old = { 40, 40 };
		monster.position.tile = monster.position.future = { 41, 40 };
		monster.direction = Direction::SouthEast;
		monster.mode = MonsterMode::MoveSouthwards;
		monster.animInfo.setNewAnimation(std::nullopt, 8, 1);
		for (int i = 0; i < frame; i++)
			monster.animInfo.processAnimation();
		const Displacement drawn = ScreenOf(monster.position.tile.x - 40, monster.position.tile.y - 40)
		    + GetOffsetForWalking(monster.animInfo, monster.direction);
		d2::SettleMonsterHitMidStep(monster);
		d2::BeginMonsterHitSlide(monster);
		const Point settled { monster.position.old.x, monster.position.old.y };
		(settled == Point { 41, 40 } ? finished : returned)++;
		const Displacement slide = d2::MonsterHitSlideOffset(monster, 0);
		// drawn the same on the hit frame
		const Displacement drawnAfter = ScreenOf(settled.x - 40, settled.y - 40) + slide;
		EXPECT_EQ(drawnAfter, drawn) << "frame " << frame;
		worstJump = std::max({ worstJump, std::abs(slide.deltaX), std::abs(slide.deltaY) * 2 });
	}
	std::printf("HITSETTLE finished=%d returned=%d worst=%d px\n", finished, returned, worstJump);
	EXPECT_GT(finished, 0);
	EXPECT_GT(returned, 0);
	EXPECT_LE(worstJump, 34) << "a monster eased more than about half a tile";
}

TEST_F(D2Harness, HitMonstersNeverFlyOffTheirStep)
{
	// All 8 walk directions, set up exactly like stock M_Walk, hit on every frame of the step: on the hit frame the
	// monster must be drawn somewhere on its own step (never pushed past either tile, e.g. into a wall) and move
	// forward frame by frame, and the ease-back must be at most half a step.
	struct WalkCase {
		Direction dir;
		MonsterMode mode;
		int dx;
		int dy;
	};
	const WalkCase walks[] = {
		{ Direction::North, MonsterMode::MoveNorthwards, -1, -1 },
		{ Direction::NorthEast, MonsterMode::MoveNorthwards, 0, -1 },
		{ Direction::East, MonsterMode::MoveSideways, 1, -1 },
		{ Direction::SouthEast, MonsterMode::MoveSouthwards, 1, 0 },
		{ Direction::South, MonsterMode::MoveSouthwards, 1, 1 },
		{ Direction::SouthWest, MonsterMode::MoveSouthwards, 0, 1 },
		{ Direction::West, MonsterMode::MoveSideways, -1, 1 },
		{ Direction::NorthWest, MonsterMode::MoveNorthwards, -1, 0 },
	};
	int cases = 0;
	int worst = 0;
	for (const WalkCase &walk : walks) {
		const Displacement step = ScreenOf(walk.dx, walk.dy);
		int lastAlong = -1;
		for (int frame = 0; frame < 8; frame++) {
			Monster monster {};
			const WorldTilePosition from { 40, 40 };
			const WorldTilePosition to { static_cast<WorldTileCoord>(40 + walk.dx), static_cast<WorldTileCoord>(40 + walk.dy) };
			monster.position.old = from;
			monster.position.future = to;
			monster.position.tile = walk.mode == MonsterMode::MoveSouthwards ? to : from;
			monster.direction = walk.dir;
			monster.mode = walk.mode;
			monster.animInfo.setNewAnimation(std::nullopt, 8, 1);
			for (int i = 0; i < frame; i++)
				monster.animInfo.processAnimation();
			d2::SettleMonsterHitMidStep(monster);
			d2::BeginMonsterHitSlide(monster);
			const Displacement slide = d2::MonsterHitSlideOffset(monster, 0);
			const Displacement drawn = ScreenOf(monster.position.old.x - 40, monster.position.old.y - 40) + slide;
			cases++;
			const std::string where = fmt::format("dir {} frame {}: drawn ({},{}) on a step to ({},{})", static_cast<int>(walk.dir), frame, drawn.deltaX, drawn.deltaY, step.deltaX, step.deltaY);
			EXPECT_GE(drawn.deltaX, std::min(0, step.deltaX)) << where;
			EXPECT_LE(drawn.deltaX, std::max(0, step.deltaX)) << where;
			EXPECT_GE(drawn.deltaY, std::min(0, step.deltaY)) << where;
			EXPECT_LE(drawn.deltaY, std::max(0, step.deltaY)) << where;
			const int along = drawn.deltaX * step.deltaX + drawn.deltaY * step.deltaY;
			EXPECT_GE(along, lastAlong) << "went backwards, " << where;
			lastAlong = along;
			EXPECT_LE(std::abs(slide.deltaX), 32) << where;
			EXPECT_LE(std::abs(slide.deltaY), 16) << where;
			worst = std::max({ worst, std::abs(slide.deltaX), std::abs(slide.deltaY) * 2 });
		}
	}
	std::printf("HITDIRS cases=%d worst=%d px\n", cases, worst);
}

TEST_F(D2Harness, WallsAreNeverEntered)
{
	ClearLevel();
	// A wall across the way with a gap at the top
	for (int y = 36; y <= 44; y++)
		Wall({ 45, y });
	Player &player = SetupHero(Start, true);
	const Point target { 50, 40 };
	d2::FreeMoveSetTarget(player, target, 0, 0, true);
	for (int tick = 0; tick < 600 && d2::FreeMoveActive(player); tick++) {
		ProcessPlayers();
		d2probe::CheckTick();
		ASSERT_FALSE(IsTileSolid(player.position.tile)) << "hero inside a wall at tick " << tick;
	}
	EXPECT_EQ(player.position.tile, target) << "hero did not find the way round";
	EXPECT_GT(d2probe::Count(d2probe::Id::path_FindPath), 0U);
}

TEST_F(D2Harness, UnreachableTargetStopsCleanly)
{
	ClearLevel();
	// Closed box around the target
	for (int i = 47; i <= 53; i++) {
		Wall({ i, 47 });
		Wall({ i, 53 });
		Wall({ 47, i });
		Wall({ 53, i });
	}
	Player &player = SetupHero(Start, true);
	d2::FreeMoveSetTarget(player, { 50, 50 }, 0, 0, true);
	const int ticks = RunHero(player);
	EXPECT_LT(ticks, 600) << "hero never gave up";
	EXPECT_FALSE(d2::FreeMoveActive(player));
	EXPECT_FALSE(IsTileSolid(player.position.tile));
	EXPECT_EQ(player._pmode, PM_STAND);
}

TEST_F(D2Harness, OccupiedTilesBlockMovement)
{
	ClearLevel();
	// A monster standing right on the line (in town every monster blocks, like a towner)
	dMonster[45][40] = 1;
	Player &player = SetupHero(Start, false);
	const Point target { 50, 40 };
	d2::FreeMoveSetTarget(player, target, 0, 0, true);
	for (int tick = 0; tick < 600 && d2::FreeMoveActive(player); tick++) {
		ProcessPlayers();
		d2probe::CheckTick();
		ASSERT_NE(player.position.tile, Point(45, 40)) << "walked through the monster";
	}
	EXPECT_EQ(player.position.tile, target) << "did not walk round the monster";

	// Someone steps into the way mid-walk: the hero stops instead of overlapping
	ClearLevel();
	Player &hero = SetupHero(Start, false);
	d2::FreeMoveSetTarget(hero, { 50, 40 }, 0, 0, true);
	for (int tick = 0; tick < 5; tick++)
		ProcessPlayers();
	dMonster[hero.position.tile.x + 1][hero.position.tile.y] = 1;
	const uint64_t blockedBefore = d2probe::Count(d2probe::Id::d2_MoveBlocked);
	RunHero(hero);
	EXPECT_EQ(dMonster[hero.position.tile.x][hero.position.tile.y], 0);
	EXPECT_GT(d2probe::Count(d2probe::Id::d2_MoveBlocked), blockedBefore);
	EXPECT_EQ(hero._pmode, PM_STAND);
}

TEST_F(D2Harness, MovementLogicCostsLittle)
{
	// The mod's own work per tick (stepping, steering towards the mouse, the camera offset) must be tiny
	// next to a frame: at 1,400 fps a frame is about 700 microseconds.
	ClearLevel();
	leveltype = DTYPE_CATHEDRAL;
	Player &player = SetupHero(Start, false);
	d2probe::ForceChecks(false);
	constexpr int Ticks = 20000;
	const auto begin = std::chrono::steady_clock::now();
	for (int tick = 0; tick < Ticks; tick++) {
		if (!d2::FreeMoveActive(player)) {
			const Point target = (tick / 300) % 2 == 0 ? Point { 60, 52 } : Point { 40, 40 };
			d2::FreeMoveSetTarget(player, target, 37, -21, true);
		}
		d2::FreeMoveTick(player);
		static_cast<void>(d2::GlideCorrection(player));
	}
	const double micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / Ticks;
	d2probe::ForceChecks(true);
	std::printf("LOGIC %.3f microseconds per tick\n", micros);
	EXPECT_LT(micros, 7.0) << "movement logic costs more than 1% of a 700 microsecond frame";
}

TEST_F(D2Harness, StockModeIsUntouched)
{
	sgOptions.Gameplay.d2Movement.SetValue(false);
	ClearLevel();
	for (int y = 36; y <= 44; y++)
		Wall({ 45, y });
	Player &player = SetupHero(Start, false);
	player.isRunning = false;
	const Point target { 50, 40 };

	// MakePlrPath fills walkpath exactly like Diablo 1
	int8_t expected[MaxPathLength];
	std::memset(expected, WALK_NONE, sizeof(expected));
	const int length = FindPath([&player](Point position) { return PosOkPlayer(player, position); }, player.position.future, target, expected);
	expected[length] = WALK_NONE;
	ClrPlrPath(player);
	MakePlrPath(player, target, true);
	EXPECT_EQ(std::memcmp(player.walkpath, expected, sizeof(expected)), 0) << "stock path differs";
	EXPECT_FALSE(d2::FreeMoveActive(player));

	d2::FreeMoveTick(player);
	EXPECT_FALSE(player.freeMove.valid) << "free movement ran in Diablo 1 mode";
	EXPECT_EQ(d2::GlideCorrection(player), Displacement {});
	d2::OnWalkFine(player, target, 0x8080);
	EXPECT_FALSE(d2::FreeMoveActive(player));
	EXPECT_EQ(D2ModGameId(GameIdDiabloFull), GameIdDiabloFull);
	EXPECT_EQ(D2ModGameId(GameIdHellfireSpawn), GameIdHellfireSpawn);

	sgOptions.Gameplay.d2Movement.SetValue(true);
	EXPECT_EQ(D2ModGameId(GameIdDiabloFull), LoadBE32("DRT2"));
}

TEST_F(D2Harness, ProbesCoverTheMovementPath)
{
	ClearLevel();
	Player &player = SetupHero(Start, true);
	d2::FreeMoveSetTarget(player, Start + Displacement { 6, 3 }, 0, 0, true);
	RunHero(player);
	for (const d2probe::Id id : { d2probe::Id::d2_FreeMoveSetTarget, d2probe::Id::d2_FreeMoveTick, d2probe::Id::d2_MoveTo,
	         d2probe::Id::d2_DirectionVector, d2probe::Id::player_ProcessPlayers,
	         d2probe::Id::player_StartStand, d2probe::Id::player_PosOkPlayer, d2probe::Id::autopickup_AutoPickup }) {
		EXPECT_GT(d2probe::Count(id), 0U) << "probe " << static_cast<int>(id) << " never fired";
	}
	d2probe::Dump("d2harness_test");
}

} // namespace
} // namespace devilution



