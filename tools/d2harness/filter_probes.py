# Part of DevilutionX-D2-Movement. Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
# Licensed under the Sustainable Use License (LICENSE.md); see NOTICE-D2MOVEMENT.md.
"""Filter D2 movement harness output into pass/fail.

Reads the run folder written by run_harness.ps1 (gtest XML, probe logs, benchmark timings) and
writes results.json and summary.md there. Hard limit: 120 s (exit 124).

Exit codes: 0 PASS, 1 FAIL, 3 MISSING_PREREQ (an expected input file is missing), 124 TIMEOUT.
"""
import json
import os
import re
import sys
import threading
import xml.etree.ElementTree as ET

EXIT_PASS, EXIT_FAIL, EXIT_MISSING, EXIT_TIMEOUT = 0, 1, 3, 124
threading.Timer(120, lambda: os._exit(EXIT_TIMEOUT)).start()

FPS_TOLERANCE = 0.95                     # modded build must reach 95% of stock FPS
SPEED_TOLERANCE = 0.01                   # average full-tick speed within 1% of stock Diablo 1

checks = []


def check(name, ok, detail='', category='general'):
    checks.append({'name': name, 'ok': bool(ok), 'detail': detail, 'category': category})


def read_probe_log(path):
    log = {'counts': {}, 'stats': {}, 'fails': [], 'levels': [], 'events': [], 'bench': None, 'demo': None, 'mode': None}
    if not os.path.exists(path):
        return None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('COUNT '):
                _, name, value = line.split(' ')
                log['counts'][name] = int(value)  # the last dump wins (exit dump comes last)
            elif line.startswith('STAT '):
                parts = line.split(' ')[1:]
                for k, v in zip(parts[::2], parts[1::2]):
                    log['stats'][k] = int(v)
            elif line.startswith('FAIL '):
                log['fails'].append(line)
            elif line.startswith('DUMP '):
                m = re.search(r'mode=(\w+)', line)
                log['mode'] = m.group(1) if m else None
            elif ' LEVEL ' in line:
                m = re.search(r'LEVEL (\d+) type=(\d+) map=(\w+) monsters=(\d+):(\w+) objects=(\d+):(\w+) items=(\d+):(\w+)', line)
                if m:
                    log['levels'].append(m.groups())
            elif ' BENCH ' in line:
                m = re.search(r'frames=(\d+) seconds=([\d.]+) fps=([\d.]+)', line)
                log['bench'] = {'frames': int(m.group(1)), 'seconds': float(m.group(2)), 'fps': float(m.group(3))}
            elif ' DEMO_RESULT ' in line:
                log['demo'] = line.split(' DEMO_RESULT ')[1]
    return log


def gtest_results(path, suite_label):
    if not os.path.exists(path):
        check(f'{suite_label}: ran', False, f'missing {os.path.basename(path)} (test crashed or timed out)', 'tests')
        return
    root = ET.parse(path).getroot()
    for case in root.iter('testcase'):
        name = f"{case.get('classname')}.{case.get('name')}"
        failures = [f.get('message', '') for f in case.findall('failure')]
        skipped = case.find('skipped') is not None or case.get('result') == 'skipped'
        if skipped:
            check(f'{suite_label}: {name}', False, 'skipped (reference input missing)', 'tests')
        else:
            check(f'{suite_label}: {name}', not failures, (failures[0][:300] if failures else ''), 'tests')


def main():
    if len(sys.argv) != 2:
        print('usage: filter_probes.py <run folder>')
        return EXIT_MISSING
    run = sys.argv[1]
    meta_path = os.path.join(run, 'steps.json')
    if not os.path.exists(meta_path):
        print('steps.json missing')
        return EXIT_MISSING
    steps = json.load(open(meta_path, encoding='utf-8-sig'))

    # Step exit codes: timeouts and crashes are failures in their own right
    for step in steps['steps']:
        code = step['exitCode']
        label = {0: 'ok', 1: 'test failures', 124: 'TIMEOUT'}.get(code, f'exit {code}')
        check(f"step {step['name']} finished", code in (0, 1), f"{label} in {step['seconds']}s", 'runtime')

    # Unit and scenario tests
    for xml_name, label in (('d2harness.xml', 'harness'), ('d2mod.xml', 'mod unit'), ('d2glide.xml', 'glide sim'),
                            ('demo_d1.xml', 'demo d1'), ('demo_d2.xml', 'demo d2')):
        gtest_results(os.path.join(run, xml_name), label)

    failed_tests = {c['name'].split('.')[-1] for c in checks if c['category'] == 'tests' and not c['ok']}

    # Parity numbers printed by the harness
    harness_out = os.path.join(run, 'd2harness.log')
    parity = []
    if os.path.exists(harness_out):
        text = open(harness_out, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'PACING (town-run|dungeon) +d=\((-?\d+),(-?\d+)\) stock=(\d+) ticks mod=(\d+) ticks', text):
            parity.append({'where': m.group(1), 'dx': int(m.group(2)), 'dy': int(m.group(3)), 'stockTicks': int(m.group(4)), 'modTicks': int(m.group(5))})
        worst = re.search(r'PACING worst=(\d+) ticks', text)
        check('pacing: every trip as long as stock Diablo 1 (within 1 tick)', worst and int(worst.group(1)) <= 1,
              f"{len(parity)} trips in 16 directions, dungeon and town jog; worst difference {worst.group(1)} tick(s)" if worst else 'no pacing output', 'parity')
        m = re.search(r'BARRELS trips=(\d+) slides=(\d+)', text)
        if m:
            check('collision: slips between barrels from every angle tried', 'SlipsBetweenBarrelsAtAnAngle' not in failed_tests,
                  f'{m.group(1)} trips through a one-tile gap, {m.group(2)} corner slides', 'parity')
        m = re.search(r'LOGIC ([\d.]+) microseconds per tick', text)
        if m:
            check('fps: movement logic under 1% of a frame', float(m.group(1)) < 7.0,
                  f'{m.group(1)} microseconds per tick (a frame at 1,400 fps is about 700)', 'fps')
        m = re.search(r'OBJECTS trips=(\d+) stuck=(\d+) corner_cuts=(\d+) slides=(\d+) repaths=(\d+)', text)
        if m:
            check('collision: never stuck among scattered chests and barrels', int(m.group(2)) == 0 and 'NeverStuckAmongScatteredObjects' not in failed_tests,
                  f'{m.group(1)} random trips stock path finding can make, {m.group(2)} stuck ({m.group(3)} corner cuts, {m.group(4)} slides, {m.group(5)} re-paths)', 'parity')
        m = re.search(r'CHESTS trips=(\d+) corner_cuts=(\d+)', text)
        if m:
            check('collision: squeezes diagonally past chests', 'SqueezesDiagonallyPastChests' not in failed_tests,
                  f'{m.group(1)} trips past two corner-touching chests, {m.group(2)} corner cuts', 'parity')
        m = re.search(r'FACING directions=(\d+) wrong=(\d+) flickering=(\d+)', text)
        if m:
            check('facing: sprite matches on-screen motion, no flicker', m.group(2) == '0' and m.group(3) == '0',
                  f'{m.group(1)} directions: {m.group(2)} wrong, {m.group(3)} flickering', 'parity')
        m = re.search(r'CHESTSIDE cases=(\d+) stuck=(\d+)', text)
        if m:
            check('collision: never stuck pressed against a chest', m.group(2) == '0' and 'NeverStuckNextToAChest' not in failed_tests,
                  f'{m.group(1)} cases of pushing into a chest from every side and corner, then another way: {m.group(2)} stuck', 'parity')
        m = re.search(r'MOUSEFACING ticks=(\d+) wrong=(\d+) worst=([\d.]+)', text)
        if m:
            check('facing: follows the mouse (held and clicking around objects)', m.group(2) == '0' and 'FacingFollowsTheMouse' not in failed_tests,
                  f'{m.group(1)} moving ticks checked, {m.group(2)} with the wrong sprite for 2+ ticks', 'parity')
        m = re.search(r'TRAPS cases=(\d+) stuck=(\d+)', text)
        if m:
            check('collision: nothing traps the hero (chests, monsters, townspeople, heroes, walls; mouse held, click, gamepad)',
                  m.group(2) == '0' and 'NothingTrapsTheHero' not in failed_tests,
                  f'{m.group(1)} walk-into-then-head-off cases: {m.group(2)} stuck', 'parity')
        m = re.search(r'GAMEPAD frames=(\d+) over 64 ticks, previews skipped=(\d+)', text)
        if m:
            check('gamepad: walk cycle keeps animating', 'GamepadWalkKeepsAnimating' not in failed_tests,
                  f'{m.group(1)} walk frames over 64 ticks; {m.group(2)} frozen-frame previews suppressed', 'parity')
        for m in re.finditer(r'STRIDE (walk|run) frames=(\d+) \(expect ~128\) footsteps=(\d+)', text):
            check(f'audio: {m.group(1)} footsteps follow Diablo 1 rules', (int(m.group(3)) == 0) if m.group(1) == 'run' else 29 <= int(m.group(3)) <= 35,
                  f'{m.group(3)} footsteps over 16 tiles, {m.group(2)} walk frames', 'audio')
        offset = re.search(r'tangent table at file offset (0x[0-9a-f]+)', text)
        if offset:
            check('D2Common.dll tangent table found and identical', True, f'file offset {offset.group(1)}', 'parity')
    else:
        check('harness output present', False, 'd2harness.log missing', 'tests')

    d1 = read_probe_log(os.path.join(run, 'probe_d1.log'))
    d2 = read_probe_log(os.path.join(run, 'probe_d2.log'))
    for label, log in (('d1', d1), ('d2', d2)):
        if log is None:
            check(f'probe log {label} present', False, 'missing', 'probes')
            continue
        check(f'demo {label}: no invariant failures', not log['fails'] and log['stats'].get('failures', 0) == 0,
              '; '.join(log['fails'][:3]) or f"{log['stats'].get('failures', 0)} failures", 'errors')
        check(f'demo {label}: ran in {label} mode', log['mode'] == label, f"probe dump says {log['mode']}", 'probes')

    if d1:
        check('stock mode: replay identical to the stock recording', d1['demo'] == 'same', f"hero compare: {d1['demo']}", 'gameplay')
        touched = {k: v for k, v in d1['counts'].items() if k in ('d2_FreeMoveSetTarget', 'd2_FreeMoveTick', 'd2_MoveTo', 'd2_OnWalkFine',
                                                                  'msg_OnWalkFine', 'msg_OnSetRun', 'd2_SendWalkToCursor', 'd2_ChanceToHit',
                                                                  'd2_PlayerFlinches', 'd2_MonsterFlinches')}
        check('stock mode: no Diablo 2 code runs', all(v == 0 for v in touched.values()),
              ', '.join(f'{k}={v}' for k, v in touched.items() if v) or 'all D2-only probes at 0', 'gameplay')
    if d2:
        c = d2['counts']
        check('d2 mode: free movement active in the real game', c.get('d2_FreeMoveTick', 0) > 0 and c.get('d2_MoveTo', 0) > 0,
              f"FreeMoveTick={c.get('d2_FreeMoveTick', 0)} MoveTo={c.get('d2_MoveTo', 0)}", 'parity')
        st = d2['stats']
        for kind, label in (('walk', 'walking (dungeon and town)'), ('run', 'jogging (town)')):
            ticks = st.get(f'{kind}_ticks', 0)
            if ticks:
                moved, expected = st.get(f'{kind}_moved', 0) / ticks, st.get(f'{kind}_expected', 0) / ticks
                check(f'd2 mode: in-game {kind} speed matches stock Diablo 1', abs(moved - expected) / expected <= SPEED_TOLERANCE,
                      f'{label}: {moved:.2f} sub-tile units/tick over {ticks} ticks (stock: {expected:.0f})', 'parity')
        check('d2 mode: combat rules stay stock', c.get('d2_ChanceToHit', 0) == 0 and c.get('d2_MonsterFlinches', 0) == 0
              and c.get('d2_PlayerFlinches', 0) == 0, 'D2 combat probes at 0', 'gameplay')

    # Same seeds must build the same levels whatever the movement mode
    if d1 and d2:
        first = lambda log: {lv[0]: lv for lv in reversed(log['levels'])}
        l1, l2 = first(d1), first(d2)
        common = sorted(set(l1) & set(l2), key=int)
        mismatched = [n for n in common if l1[n][2] != l2[n][2] or l1[n][4] != l2[n][4] or l1[n][6] != l2[n][6]]
        check('level generation identical in both modes (map, monsters, objects)', common and not mismatched,
              f"levels compared: {', '.join(common)}" + (f"; differ: {', '.join(mismatched)}" if mismatched else ''), 'subsystems')
        subsystems = {}
        for key in ('sound_PlaySFX', 'sound_PlaySfxLoc', 'missiles_MonsterMHit', 'missiles_PlayerMHit', 'monster_MonsterAttackPlayer',
                    'monster_M_StartHit', 'player_PlrHitMonst', 'player_StartPlrHit', 'lighting_ChangeLightXY', 'lighting_ChangeVisionXY',
                    'autopickup_AutoPickup', 'trigs_CheckTriggers', 'diablo_LoadGameLevel', 'monster_UpdateEnemy', 'path_FindPath'):
            subsystems[key] = {'d1': d1['counts'].get(key, 0), 'd2': d2['counts'].get(key, 0)}
        for key in ('sound_PlaySFX', 'sound_PlaySfxLoc', 'monster_UpdateEnemy', 'trigs_CheckTriggers', 'lighting_ChangeVisionXY'):
            check(f'subsystem alive in d2 mode: {key}', subsystems[key]['d2'] > 0, f"d1={subsystems[key]['d1']} d2={subsystems[key]['d2']}", 'subsystems')
    else:
        subsystems = {}

    # FPS benchmark (timedemo with rendering; best of the runs)
    bench = steps.get('bench', {})
    fps = {k: max(v) for k, v in bench.items() if v}
    if 'stock' in fps:
        # Stock movement replays the whole demo, so whole-run FPS compares like with like
        if 'mod_d1' in fps:
            check('fps: mod (D1 movement) not below 95% of stock', fps['mod_d1'] >= FPS_TOLERANCE * fps['stock'],
                  f"{fps['mod_d1']:.1f} fps vs stock {fps['stock']:.1f} fps ({100 * fps['mod_d1'] / fps['stock']:.1f}%)", 'fps')
        else:
            check('fps: mod (D1 movement) measured', False, 'no benchmark result', 'fps')
        # With D2 movement the replay diverges and ends somewhere else, so compare the same stretch of frames
        # (timing marks every 200 frames) against the D1 run of the same exe, which itself matches stock
        marks = {}
        for name in os.listdir(run):
            m = re.match(r'bench_(mod_d1|mod_d2)_\d+$', name)
            out = os.path.join(run, name, 'output.log')
            if m and os.path.exists(out):
                for f, s in re.findall(r'timedemo mark (\d+) frames ([\d.]+) seconds', open(out, encoding='utf-8', errors='replace').read()):
                    key = (m.group(1), int(f))
                    marks[key] = min(marks.get(key, 1e9), float(s))
        common = sorted({f for mode, f in marks if mode == 'mod_d1'} & {f for mode, f in marks if mode == 'mod_d2'})
        # The D2 replay plays a different game (the hero may die, reload or sit in a menu while frames keep being
        # drawn), so only compare the stretch before its first level change, where both draw the same level
        limit = None
        if d2:
            later = [int(t) for t in re.findall(r'^T(\d+) LEVEL', open(os.path.join(run, 'probe_d2.log'), encoding='utf-8').read(), re.M) if int(t) > 0]
            limit = min(later) if later else None
        window = [f for f in common if limit is None or f <= limit]
        if window:
            frames = window[-1]
            d1fps, d2fps = frames / marks[('mod_d1', frames)], frames / marks[('mod_d2', frames)]
            fps['mod_d1_window'], fps['mod_d2_window'], fps['window_frames'] = d1fps, d2fps, frames
            check('fps: mod (D2 movement) not below 95% over the same frames', d2fps >= FPS_TOLERANCE * d1fps,
                  f'first {frames} frames (same level in both replays): {d2fps:.1f} fps vs {d1fps:.1f} fps with stock movement ({100 * d2fps / d1fps:.1f}%)', 'fps')
        else:
            check('fps: mod (D2 movement) measured', False, 'no common timing marks', 'fps')
    elif any(bench.values()):
        check('fps: stock baseline measured', False, 'stock exe did not report FPS', 'fps')
    for name, lines in steps.get('errors', {}).items():
        check(f'errors: none in {name} output', not lines, '; '.join(lines[:3]), 'errors')

    passed = all(c['ok'] for c in checks)
    result = {'pass': passed, 'checks': checks, 'parity': parity, 'subsystems': subsystems, 'fps': fps,
              'speeds': {'d2': d2['stats'] if d2 else {}}}
    json.dump(result, open(os.path.join(run, 'results.json'), 'w'), indent=1)

    lines = [f"# D2 movement harness: {'PASS' if passed else 'FAIL'}", '',
             f"{sum(c['ok'] for c in checks)}/{len(checks)} checks passed", '', '| | Check | Detail |', '|---|---|---|']
    for c in checks:
        lines.append(f"| {'PASS' if c['ok'] else '**FAIL**'} | {c['name']} | {c['detail'].replace('|', '/')} |")
    open(os.path.join(run, 'summary.md'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    for c in checks:
        print(f"{'PASS' if c['ok'] else 'FAIL'}  {c['name']}  {c['detail']}")
    print('RESULT', 'PASS' if passed else 'FAIL')
    return EXIT_PASS if passed else EXIT_FAIL


if __name__ == '__main__':
    code = main()
    sys.stdout.flush()
    os._exit(code)
