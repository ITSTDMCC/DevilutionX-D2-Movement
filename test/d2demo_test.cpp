/**
 * Full game replay for the Diablo 2 movement harness: plays the recorded WarriorLevel1to2 demo
 * headless (real level generation, monsters, items, missiles, sound calls) with the movement mode
 * picked by D2_DEMO_MODE, and writes probe output to D2PROBE_LOG.
 *
 *   D2_DEMO_MODE=d1  stock movement: the hero must end up exactly as in the stock recording
 *   D2_DEMO_MODE=d2  Diablo 2 movement: the replayed clicks steer a faster hero, so the outcome
 *                    differs, but the game must run to the end with no probe invariant failures
 *   D2_DEMO_DIR      a scratch copy of test/fixtures/timedemo/WarriorLevel1to2 (the run writes saves there)
 *
 * Part of DevilutionX-D2-Movement. Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
 * Licensed under the Sustainable Use License (LICENSE.md); see NOTICE-D2MOVEMENT.md.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "d2mod.h"
#include "d2probe.h"
#include "diablo.h"
#include "engine/demomode.h"
#include "options.h"
#include "pfile.h"
#include "utils/display.h"
#include "utils/paths.h"

using namespace devilution;

namespace {

bool Dummy_GetHeroInfo(_uiheroinfo *pInfo)
{
	return true;
}

TEST(D2Demo, ReplayWarriorLevel1to2)
{
	const char *dir = std::getenv("D2_DEMO_DIR");
	const char *modeEnv = std::getenv("D2_DEMO_MODE");
	if (dir == nullptr || modeEnv == nullptr)
		GTEST_SKIP() << "D2_DEMO_DIR and D2_DEMO_MODE must be set (tools/d2harness/run_harness.ps1 does this)";
	const std::string mode = modeEnv;
	ASSERT_TRUE(mode == "d1" || mode == "d2") << "D2_DEMO_MODE must be d1 or d2";

	paths::SetPrefPath(dir);
	paths::SetConfigPath(dir);
	LoadCoreArchives();
	LoadGameArchives();
	ASSERT_TRUE(HaveSpawn() || HaveDiabdat()) << "needs spawn.mpq or diabdat.mpq next to the test";

	InitKeymapActions();
	LoadOptions();
	sgOptions.Gameplay.d2Movement.SetValue(mode == "d2");
	sgOptions.Gameplay.d2Combat.SetValue(false);

	const int demoNumber = 0;
	Players.resize(1);
	MyPlayerId = demoNumber;
	MyPlayer = &Players[MyPlayerId];
	*MyPlayer = {};

	gbIsSpawn = true;
	gbIsHellfire = false;
	gbMusicOn = false;
	gbSoundOn = false;
	HeadlessMode = true;
	demo::InitPlayBack(demoNumber, true);

	pfile_ui_set_hero_infos(Dummy_GetHeroInfo);
	gbLoadGame = true;

	demo::OverrideOptions();
	AdjustToScreenGeometry(forceResolution);

	StartGame(false, true);

	EXPECT_EQ(d2::MovementEnabled(), mode == "d2");
	if (mode == "d1") {
		// The recording ends by saving and quitting through the menu, so there is a final hero to compare
		HeroCompareResult result = pfile_compare_hero_demo(demoNumber, true);
		d2probe::Event(std::string("DEMO_RESULT ") + (result.status == HeroCompareResult::Same ? "same" : "different"));
		EXPECT_EQ(result.status, HeroCompareResult::Same) << "stock movement must reproduce the stock recording exactly: " << result.message;
	} else {
		// The faster hero is elsewhere when the recorded menu clicks arrive, so the replay ends the game itself
		d2probe::Event("DEMO_RESULT diverged (expected with D2 movement)");
	}
	d2probe::Dump("demo");
	EXPECT_EQ(d2probe::FailureCount(), 0U) << "probe invariants failed, see the probe log";
	ASSERT_FALSE(gbRunGame);
	gbRunGame = false;
	init_cleanup();
}

} // namespace
