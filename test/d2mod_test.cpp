#include <gtest/gtest.h>

#include "d2mod.h"
#include "player.h"

namespace devilution {
namespace {

Point Follow(Point start, const int8_t *path, int length)
{
	for (int i = 0; i < length; i++)
		start += d2::WalkStepDisplacement(path[i]);
	return start;
}

TEST(D2ModTest, ChanceToHitMatchesDiablo2Formula)
{
	// Equal rating and defense at equal level: 200% * 1/2 * 1/2 = 50%
	EXPECT_EQ(d2::ChanceToHit(100, 100, 10, 10), 50);
	// No defense: 200% * 1 * 1/2 = 100%, clamped to 95%
	EXPECT_EQ(d2::ClampChanceToHit(d2::ChanceToHit(100, 0, 10, 10)), 95);
	// Huge defense: clamped to 5%
	EXPECT_EQ(d2::ClampChanceToHit(d2::ChanceToHit(10, 10000, 1, 50)), 5);
}

TEST(D2ModTest, StraightenPathInterleavesSteps)
{
	const Point start { 20, 20 };
	// What A* gives: two diagonal steps first, then four straight ones (a dogleg)
	int8_t path[MaxPathLength] = { WALK_E, WALK_E, WALK_SE, WALK_SE, WALK_SE, WALK_SE };
	const Point end = Follow(start, path, 6);

	int length = d2::StraightenPath([](Point) { return true; }, start, path, 6);

	ASSERT_EQ(length, 6);
	EXPECT_EQ(Follow(start, path, length), end);
	int eastSteps = 0;
	int longestRun = 0;
	int run = 0;
	for (int i = 0; i < length; i++) {
		if (path[i] == WALK_E) {
			eastSteps++;
			run = 0;
		} else {
			EXPECT_EQ(path[i], WALK_SE);
			longestRun = std::max(longestRun, ++run);
		}
	}
	EXPECT_EQ(eastSteps, 2);
	EXPECT_LE(longestRun, 2) << "Straight steps should be spread between the diagonal ones";
}

TEST(D2ModTest, StraightenPathAvoidsBlockedTiles)
{
	const Point start { 20, 20 };
	int8_t path[MaxPathLength] = { WALK_E, WALK_E, WALK_SE, WALK_SE, WALK_SE, WALK_SE };
	const Point end = Follow(start, path, 6);
	// Block every tile the straight line would use, leaving only the original route open
	Point original[7];
	original[0] = start;
	for (int i = 0; i < 6; i++)
		original[i + 1] = original[i] + d2::WalkStepDisplacement(path[i]);
	auto posOk = [&](Point p) {
		for (Point o : original) {
			if (o == p)
				return true;
		}
		return false;
	};

	int length = d2::StraightenPath(posOk, start, path, 6);

	EXPECT_EQ(Follow(start, path, length), end);
	Point p = start;
	for (int i = 0; i < length; i++) {
		p += d2::WalkStepDisplacement(path[i]);
		EXPECT_TRUE(posOk(p)) << "Step " << i << " walks onto a blocked tile";
	}
}

TEST(D2ModTest, StraightenPathLeavesShortPathsAlone)
{
	int8_t path[MaxPathLength] = { WALK_N, WALK_E };
	EXPECT_EQ(d2::StraightenPath([](Point) { return true; }, { 20, 20 }, path, 2), 2);
	EXPECT_EQ(path[0], WALK_N);
	EXPECT_EQ(path[1], WALK_E);
}

} // namespace
} // namespace devilution
