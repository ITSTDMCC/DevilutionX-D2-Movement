/**
 * Headless harness for the Diablo 2 movement mod: movement parity with the Diablo 2 reference,
 * collision, stock-mode parity and probe coverage. Nothing is drawn; heroes are driven through the
 * real ProcessPlayers loop on a synthetic level, and the per-tick probe invariants run throughout.
 *
 * Environment (set by tools/d2harness/run_harness.ps1):
 *   D2_COMMON_DLL  path to Diablo 2 1.12 D2Common.dll, for checking the tangent table byte for byte
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "options.h"
#include "player.h"

namespace devilution {
namespace {

constexpr Point Start { 40, 40 };

void ClearLevel()
{
	std::memset(dPlayer, 0, sizeof(dPlayer));
	std::memset(dMonster, 0, sizeof(dMonster));
	std::memset(dPiece, 0, sizeof(dPiece));
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
	Players.resize(1);
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

/** Seconds Diablo 2 needs to move a unit by (dx, dy) Diablo 1 tiles = (2dx, 2dy) subtiles. */
double RefTravelSeconds(const std::vector<RefEntry> &lut, int dx, int dy, int velocity)
{
	const int64_t speed = (1024LL * (velocity << 8)) >> 6;
	int64_t x = 0, y = 0;
	const int64_t tx = 2LL * dx * 65536, ty = 2LL * dy * 65536;
	int frames = 0;
	while (frames < 100000) {
		const double remaining = std::hypot(static_cast<double>(tx - x), static_cast<double>(ty - y));
		if (remaining <= speed) {
			// Arrives part way through this frame
			return (frames + remaining / speed) / 25.0;
		}
		const RefHeading h = RefDirection(lut, x, y, tx, ty);
		x += (speed * h.x) >> 12;
		y += (speed * h.y) >> 12;
		frames++;
	}
	return -1;
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

TEST_F(D2Harness, WalkAndRunSpeedsMatchDiablo2)
{
	size_t offset = 0;
	const std::vector<RefEntry> lut = ReadD2CommonTable(offset);
	double worstPercent = 0;
	for (int running = 0; running < 2; running++) {
		for (const SpeedCase &c : SpeedCases) {
			ClearLevel();
			Player &player = SetupHero(Start, running != 0);
			const Point target = Start + Displacement { c.dx, c.dy };
			d2::FreeMoveSetTarget(player, target, 0, 0, true);
			int ticks = 0;
			double lastTickFraction = 0;
			for (; ticks < 600 && d2::FreeMoveActive(player); ticks++) {
				const int32_t beforeX = player.freeMove.x, beforeY = player.freeMove.y;
				ProcessPlayers();
				d2probe::CheckTick();
				if (!d2::FreeMoveActive(player)) {
					const double moved = std::hypot(player.freeMove.x - beforeX, player.freeMove.y - beforeY);
					lastTickFraction = moved / (running != 0 ? d2::RunSpeed : d2::WalkSpeed);
				}
			}
			ASSERT_EQ(player.position.tile, target) << c.dx << "," << c.dy;
			const double seconds = (ticks - 1 + lastTickFraction) / 20.0;
			const double tiles = std::hypot(c.dx, c.dy);
			const double expected = tiles / (running != 0 ? 7.03125 : 4.6875);
			double reference = expected;
			if (!lut.empty())
				reference = RefTravelSeconds(lut, c.dx, c.dy, running != 0 ? d2::D2RunVelocity : d2::D2WalkVelocity);
			const double percent = 100.0 * std::abs(seconds - reference) / reference;
			worstPercent = std::max(worstPercent, percent);
			std::printf("PARITY %s d=(%d,%d) d1mod=%.4fs d2ref=%.4fs diff=%.3f%% speed=%.3f tiles/s\n",
			    running != 0 ? "run " : "walk", c.dx, c.dy, seconds, reference, percent, tiles / seconds);
			EXPECT_LE(percent, 1.0) << "travel time differs from Diablo 2 by more than 1%";
			EXPECT_EQ(player._pmode, PM_STAND);
			EXPECT_FALSE(player.freeMove.animating) << "hero kept the walk animation after stopping";
		}
	}
	std::printf("PARITY worst=%.3f%% reference=%s\n", worstPercent, lut.empty() ? "formula" : "D2Common.dll table");
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
	for (int running = 0; running < 2; running++) {
		ClearLevel();
		Player &player = SetupHero(Start, running != 0);
		const uint64_t stepsBefore = d2probe::Count(d2probe::Id::sound_PlaySfxLoc);
		d2::FreeMoveSetTarget(player, Start + Displacement { 16, 0 }, 0, 0, true);
		int frames = 0;
		int8_t last = player.AnimInfo.currentFrame;
		for (int tick = 0; tick < 600 && d2::FreeMoveActive(player); tick++) {
			ProcessPlayers();
			const int8_t now = player.AnimInfo.currentFrame;
			frames += (now - last + 8) % 8;
			last = now;
		}
		const uint64_t steps = d2probe::Count(d2probe::Id::sound_PlaySfxLoc) - stepsBefore;
		// Diablo 1's walk cycle covers one tile per 8 frames; 16 tiles should show about 128 frames
		std::printf("STRIDE %s frames=%d (expect ~128) footsteps=%llu\n", running != 0 ? "run" : "walk", frames, static_cast<unsigned long long>(steps));
		EXPECT_NEAR(frames, 128, 10);
		if (running != 0) {
			EXPECT_EQ(steps, 0U) << "Diablo 1 plays no footsteps while running";
		} else {
			// Frames 0 and 4 of each cycle: two per tile
			EXPECT_NEAR(static_cast<double>(steps), 32.0, 3.0);
		}
	}
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
